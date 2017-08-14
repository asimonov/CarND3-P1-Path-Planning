#include <fstream>
#include <uWS/uWS.h>
#include <iomanip>
#include <thread>
#include <vector>

#include "json.hpp"

#include "log_utils.h"
#include "coordinate_utils.h"

#include "Route.h"
#include "Car.h"
#include "BehaviourPlanner.h"
#include "PathPlanner.h"

using namespace std;
using json = nlohmann::json;


// global variable. initialised in main. used in onMessage
Route route;


// process telemetery event
void onMessage(uWS::WebSocket<uWS::SERVER> ws,
               char *data,
               size_t length,
               uWS::OpCode opCode)
{
  // "42" at the start of the message means there's a websocket message event.
  // The 4 signifies a websocket message
  // The 2 signifies a websocket event
  if (length && length > 2 && data[0] == '4' && data[1] == '2') {

    // Checks if the SocketIO event has JSON data.
    string data_s = string(data);
    string s = "";
    auto found_null = data_s.find("null");
    auto b1 = data_s.find_first_of("[");
    auto b2 = data_s.find_first_of("}");
    if (b1 != string::npos && b2 != string::npos) {
      s = data_s.substr(b1, b2 - b1 + 2);
    }
    if (s != "") {
      auto j = json::parse(s);

      string event = j[0].get<string>();

      if (event == "telemetry") {
        // j[1] is the data JSON object

        // Main car's localization Data
        double car_x = j[1]["x"];
        double car_y = j[1]["y"];
        double car_s = j[1]["s"];
        double car_d = j[1]["d"];
        double car_yaw = j[1]["yaw"];
        double car_speed = j[1]["speed"];

        // Previous path data given to the Planner
        auto previous_path_x = j[1]["previous_path_x"];
        auto previous_path_y = j[1]["previous_path_y"];

        // Previous path's end s and d values
        double end_path_s = j[1]["end_path_s"];
        double end_path_d = j[1]["end_path_d"];

        // Sensor Fusion Data, a list of all other cars on the same side of the road.
        auto sensor_fusion = j[1]["sensor_fusion"];
        //["sensor_fusion"] A 2d vector of cars and then that car's
        // [car's unique ID,
        //  car's x position in map coordinates,
        //  car's y position in map coordinates,
        //  car's x velocity in m/s,
        //  car's y velocity in m/s,
        //  car's s position in frenet coordinates,
        //  car's d position in frenet coordinates.]

        // log event
        auto fr = route.get_frenet(car_x, car_y, car_yaw);
        cout << ts_ms_str() << "IN  n="<<setw(5)<<previous_path_x.size()
             <<" x  ="<<setw(8)<<car_x  <<" y  ="<<setw(8)<<car_y
             <<" s="<<car_s<<"(mine: "<<fr[0]<<")"<<" d="<<car_d<<"(mine:"<<fr[1]<<")"
             <<" yaw="<<car_yaw <<" v="<<car_speed
             << endl;
        // replace frenet with my own frenet estimates
        car_s = fr[0];
        car_d = fr[1];

        // planning constants
        const double dt_s = 0.02; // discretisation time length, in seconds
        const int    num_lanes = 3; // number of lanes we have
        const double lane_width = 4.0; // highway lane width, in meters
        const double time_horizon_s = 5.0; // min planning time horizon, in seconds
        const double max_speed = mph2ms(50.0); // max speed in meter/second
        const double target_speed = mph2ms(45.0); // target speed in meter/second
        const double max_acceleration = 10.0; // maximum acceleration, in m/s2
        const double max_jerk = 10.0; // maximum jerk, in m/s3

        // define existing trajectory
        Trajectory in_traj(previous_path_x, previous_path_y, dt_s);
        // save debug trajectory info to a file
        std::stringstream ss;
        ss << "in_traj_" << ts_ms();
        in_traj.dump_to_file(ss.str());

        // possibly cut old trajectory shorter size
//        const double keep_old_trajectory_secs = 50.0;
//        vector<double> x, y;
//        double i=0;
//        while (i<previous_path_x.size() && i*dt_s <= keep_old_trajectory_secs)
//        {
//          x.push_back(previous_path_x[i]);
//          y.push_back(previous_path_y[i]);
//          i++;
//        }
//        in_traj = Trajectory(x,y,dt_s);

        // define ego car object
        double car_acceleration = 0.0;
        if (in_traj.getSize())
        {
          auto xy = in_traj.getFinalXY();
          car_x = xy[0];
          car_y = xy[1];
          car_yaw = in_traj.getFinalYaw();
          car_speed = in_traj.getFinalSpeed();
          car_acceleration = in_traj.getFinalAcceleration();
          auto fr = route.get_frenet(car_x, car_y, car_yaw);
          car_s = fr[0];
          car_d = fr[1];
        }
        int car_lane = car_d / lane_width;
        Car ego(Car::getEgoID(), car_x, car_y, deg2rad(car_yaw), car_s, car_d, car_lane, car_speed, car_acceleration, target_speed);
        double ego_time = in_traj.getTotalT();

        // process sensor fusion and define all other cars on the road
        vector<Car> other_cars;
        for (int i = 0; i < sensor_fusion.size(); i++) {
          int id = sensor_fusion[i][0];
          assert(id!=Car::getEgoID());
          double x = sensor_fusion[i][1];
          double y = sensor_fusion[i][2];

          double vx = sensor_fusion[i][3];
          double vy = sensor_fusion[i][4];
          double v = sqrt(vx*vx+vy*vy); // assuming it is all in direction of s, none in d
          double yaw = atan2(vy,vx);

          double s = sensor_fusion[i][5];
          double d = sensor_fusion[i][6];
          auto fr = route.get_frenet(x, y, yaw);
          s = fr[0];
          d = fr[1];
          int lane = d / lane_width;
          double a = 0.0;
          double v_target = v;
          Car other_car_at_zero(id, x, y, yaw, s, d, lane, v, a, v_target);
          // now create state of other cars as of end time of ego trajectory
          other_cars.push_back(other_car_at_zero.advance(ego_time));
        }

        // use behaviour planner to decide on high level what maneuvre to execute and where to go to,
        // with what speed and acceleration
        BehaviourPlanner bp(num_lanes, lane_width, ego, other_cars);
        Car planned_ego_state = bp.plan(time_horizon_s, max_speed, max_acceleration, max_jerk);

        // plan trajectory (x,y points spaced at dt_s)
        JMTPlanner planner;
        Trajectory out_tr = in_traj;
        double t = in_traj.getTotalT();
        if (t < 5.0) {
          cout << "t="<< out_tr.getTotalT() << "(n="<<out_tr.getSize()<<") extending.." << endl;
          out_tr = planner.extendTrajectory(ego, in_traj, route, time_horizon_s, target_speed, max_speed, max_acceleration, max_jerk);
          std::stringstream ss2;
          ss2 << "out_traj_" << ts_ms();
          out_tr.dump_to_file(ss2.str());
        }


        // send control message back to the simulator
        json msgJson;
        msgJson["next_x"] = out_tr.getX();
        msgJson["next_y"] = out_tr.getY();
        auto msg = "42[\"control\"," + msgJson.dump() + "]";

        // sleep for 100 ms, to match real cars latency
        this_thread::sleep_for(chrono::milliseconds(100));

        int n = out_tr.getY().size();
        cout << ts_ms_str() << "OUT n="<<setw(5)<<n
             << " x_s="<<setw(8)<<out_tr.getX()[0] <<" y_s="<<setw(8)<<out_tr.getY()[0]
             << " x_f="<<out_tr.getX()[n-1] << " y_f="<<out_tr.getY()[n-1]
             << endl;
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    } else {
      // Manual driving
      std::string msg = "42[\"manual\",{}]";
      ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
    }
  }
}




// main.
// just setup webserver listening and provide handlers to do the job. then start listen/reply loop
int main() {
  uWS::Hub h;

  // read waypoints data from file
  route.read_data("../data/highway_map.csv");
  // debug output
  ofstream f("track_debug.csv", ofstream::out);
  double d = 6.0;
  auto prev_xy = route.get_XY(0.0, d);
  double ds = 0.5;
  for (double s=ds; s<route.get_max_s()+2.; s=s+ds)
  {
    auto xy = route.get_XY(s, d);
    double yaw = angle(prev_xy[0], prev_xy[1], xy[0], xy[1]);
    auto fr = route.get_frenet(xy[0], xy[1],yaw);
    auto xy2 = route.get_XY(fr[0],fr[1]);
    f<<s<<" "<<xy[0]<<" "<<xy[1]<<" "<<rad2deg(yaw)<<" "<<fr[0]<<" "<<fr[1]<<" "<<xy2[0]<<" "<<xy2[1]<<endl;
    prev_xy = xy;
  }
  f.close();
  //auto xy = route.get_XY(	6938.5, 6.0);
  //auto fr = route.get_frenet(xy[0], xy[1], deg2rad(-2));
  //auto fr = route.get_frenet(930.947, 1129.04, 0);

  h.onMessage(onMessage);

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
      const std::string s = "<h1>Hello world!</h1>";
      if (req.getUrl().valueLength == 1) {
        res->end(s.data(), s.length());
      } else {
        // i guess this should be done more gracefully?
        res->end(nullptr, 0);
      }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
      std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
      ws.close();
      std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
















































































