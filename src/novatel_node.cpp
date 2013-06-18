/*
 * The MIT License (MIT)
 * Copyright (c) 2012 William Woodall <wjwwood@gmail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a 
 * copy of this software and associated documentation files (the "Software"), 
 * to deal in the Software without restriction, including without limitation 
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, 
 * and/or sell copies of the Software, and to permit persons to whom the 
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included 
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <iostream>
#include <sstream>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <string>

#include <ros/ros.h>
#include <tf/tf.h>
#include <gps_msgs/Ephemeris.h>
#include <gps_msgs/DualBandRange.h>

#ifdef WIN32
 #ifdef DELETE
 // ach, windows.h polluting everything again,
 // clashes with autogenerated visualization_msgs/Marker.h
 #undef DELETE
 #endif
#endif
#include "nav_msgs/Odometry.h"
#include "sensor_msgs/NavSatFix.h"

#include <boost/tokenizer.hpp>

#include "novatel/novatel.h"
using namespace novatel;

// Logging system message handlers
void handleInfoMessages(const std::string &msg) {ROS_INFO("%s",msg.c_str());}
void handleWarningMessages(const std::string &msg) {ROS_WARN("%s",msg.c_str());}
void handleErrorMessages(const std::string &msg) {ROS_ERROR("%s",msg.c_str());}
void handleDebugMessages(const std::string &msg) {ROS_DEBUG("%s",msg.c_str());}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static double radians_to_degrees = 180.0 / M_PI;
static double degrees_to_radians = M_PI / 180.0;
static double degrees_square_to_radians_square = degrees_to_radians*degrees_to_radians;

static double sigma_v = 0.05; // velocity std dev in m/s

// ROS Node class
class NovatelNode {
public:
  NovatelNode() : nh_("~"){

    // set up logging handlers
    gps_.setLogInfoCallback(handleInfoMessages);
    gps_.setLogWarningCallback(handleWarningMessages);
    gps_.setLogErrorCallback(handleErrorMessages);
    gps_.setLogDebugCallback(handleDebugMessages);

    gps_.set_best_utm_position_callback(boost::bind(&NovatelNode::BestUtmHandler, this, _1, _2));
    gps_.set_best_velocity_callback(boost::bind(&NovatelNode::BestVelocityHandler, this, _1, _2));
    gps_.set_ins_position_velocity_attitude_short_callback(boost::bind(&NovatelNode::InsPvaHandler, this, _1, _2));
    gps_.set_ins_covariance_short_callback(boost::bind(&NovatelNode::InsCovHandler, this, _1, _2));
    gps_.set_raw_imu_short_callback(boost::bind(&NovatelNode::RawImuHandler, this, _1, _2));
    gps_.set_receiver_hardware_status_callback(boost::bind(&NovatelNode::HardwareStatusHandler, this, _1, _2));

    gps_.set_gps_ephemeris_callback(boost::bind(&NovatelNode::EphemerisHandler, this, _1, _2));
    gps_.set_range_measurements_callback(boost::bind(&NovatelNode::RangeHandler, this, _1, _2));
  }

  ~NovatelNode() {
    this->disconnect();
  }

  void BestUtmHandler(UtmPosition &pos, double &timestamp) {
    ROS_DEBUG("Received BestUtm");

    sensor_msgs::NavSatFix sat_fix;
    sat_fix.header.stamp = ros::Time::now();
    sat_fix.header.frame_id = "/odom";

    if (pos.position_type == NONE)
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_NO_FIX;
    else if ((pos.position_type == WAAS) || 
             (pos.position_type == OMNISTAR) ||   
             (pos.position_type == OMNISTAR_HP) || 
             (pos.position_type == OMNISTAR_XP) || 
             (pos.position_type == CDGPS))
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_SBAS_FIX;
    else if ((pos.position_type == PSRDIFF) || 
             (pos.position_type == NARROW_FLOAT) ||   
             (pos.position_type == WIDE_INT) ||     
             (pos.position_type == WIDE_INT) ||     
             (pos.position_type == NARROW_INT) ||     
             (pos.position_type == RTK_DIRECT_INS) ||     
             (pos.position_type == INS_PSRDIFF) ||    
             (pos.position_type == INS_RTKFLOAT) ||   
             (pos.position_type == INS_RTKFIXED))
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_GBAS_FIX;
     else 
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_FIX;

    if (pos.signals_used_mask & 0x30)
      sat_fix.status.service = sensor_msgs::NavSatStatus::SERVICE_GLONASS;
    else
      sat_fix.status.service = sensor_msgs::NavSatStatus::SERVICE_GPS;

    // TODO: convert positon to lat, long, alt to export

    // TODO: add covariance
    // covariance is east,north,up in row major form


    sat_fix.position_covariance_type = sensor_msgs::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;

    nav_sat_fix_publisher_.publish(sat_fix);

    nav_msgs::Odometry cur_odom_;
    cur_odom_.header.stamp = sat_fix.header.stamp;
    cur_odom_.header.frame_id = "/odom";
    cur_odom_.pose.pose.position.x = pos.easting;
    cur_odom_.pose.pose.position.y = pos.northing;
    cur_odom_.pose.pose.position.z = pos.height;
    // covariance representation given in REP 103
    //http://www.ros.org/reps/rep-0103.html#covariance-representation
    // (x, y, z, rotation about X axis, rotation about Y axis, rotation about Z axis)
    // row major
    cur_odom_.pose.covariance[0] = pos.easting_standard_deviation * pos.easting_standard_deviation;
    cur_odom_.pose.covariance[7] = pos.northing_standard_deviation * pos.northing_standard_deviation;
    cur_odom_.pose.covariance[14] = pos.height_standard_deviation * pos.height_standard_deviation;
    // have no way of knowing roll and pitch with just GPS
    cur_odom_.pose.covariance[21] = DBL_MAX;
    cur_odom_.pose.covariance[28] = DBL_MAX;

    // see if there is a recent velocity message
    if ((cur_velocity_.header.gps_week==pos.header.gps_week) 
         && (cur_velocity_.header.gps_millisecs==pos.header.gps_millisecs)) 
    {
      cur_odom_.twist.twist.linear.x=cur_velocity_.horizontal_speed*cos(cur_velocity_.track_over_ground);
      cur_odom_.twist.twist.linear.y=cur_velocity_.horizontal_speed*sin(cur_velocity_.track_over_ground);
      cur_odom_.twist.twist.linear.z=cur_velocity_.vertical_speed;

      cur_odom_.pose.pose.orientation = tf::createQuaternionMsgFromYaw(
          cur_velocity_.track_over_ground*degrees_to_radians);

      // if i have a fix, velocity std, dev is constant
      if (cur_velocity_.position_type>NONE) {
        // yaw covariance
        double heading_std_dev=sigma_v/cur_velocity_.horizontal_speed;
        cur_odom_.pose.covariance[35] = heading_std_dev * heading_std_dev;
        // x and y velocity covariance
        cur_odom_.twist.covariance[0] = sigma_v*sigma_v;
        cur_odom_.twist.covariance[7] = sigma_v*sigma_v;
      } else {
        cur_odom_.pose.covariance[35] = DBL_MAX;
        cur_odom_.twist.covariance[0] = DBL_MAX;
        cur_odom_.twist.covariance[7] = DBL_MAX;
      }

    }

    odom_publisher_.publish(cur_odom_);
      

  }

  void BestVelocityHandler(Velocity &vel, double &timestamp) {
    ROS_DEBUG("Received BestVel");
    cur_velocity_ = vel;

  }

  void InsPvaHandler(InsPositionVelocityAttitudeShort &ins_pva, double &timestamp) {
    // convert pva position to UTM
    double northing, easting;
    int zoneNum;
    bool north;

    gps_.ConvertLLaUTM(ins_pva.latitude, ins_pva.longitude, &northing, &easting, &zoneNum, &north);

    sensor_msgs::NavSatFix sat_fix;
    sat_fix.header.stamp = ros::Time::now();
    sat_fix.header.frame_id = "/odom";

    if (ins_pva.status == INS_SOLUTION_GOOD)
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_FIX;
    else 
      sat_fix.status.status = sensor_msgs::NavSatStatus::STATUS_NO_FIX;

    sat_fix.status.service = sensor_msgs::NavSatStatus::SERVICE_GPS;

    sat_fix.latitude = ins_pva.latitude;
    sat_fix.longitude = ins_pva.longitude;
    sat_fix.altitude = ins_pva.height;

    sat_fix.position_covariance_type = sensor_msgs::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;

    nav_sat_fix_publisher_.publish(sat_fix);

    nav_msgs::Odometry cur_odom_;
    cur_odom_.header.stamp = sat_fix.header.stamp;
    cur_odom_.header.frame_id = "/odom";
    cur_odom_.pose.pose.position.x = easting;
    cur_odom_.pose.pose.position.y = northing;
    cur_odom_.pose.pose.position.z = ins_pva.height;
    cur_odom_.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(ins_pva.roll,ins_pva.pitch,ins_pva.azimuth);

    //cur_odom_->pose.covariance[0] = 

    cur_odom_.twist.twist.linear.x=ins_pva.east_velocity;
    cur_odom_.twist.twist.linear.y=ins_pva.north_velocity;
    cur_odom_.twist.twist.linear.z=ins_pva.up_velocity;
      // TODO: add covariance

    // see if there is a matching ins covariance message
    if ((cur_ins_cov_.gps_week==ins_pva.gps_week) 
         && (cur_ins_cov_.gps_millisecs==ins_pva.gps_millisecs)) {

      cur_odom_.pose.covariance[0] = cur_ins_cov_.position_covariance[0];
      cur_odom_.pose.covariance[1] = cur_ins_cov_.position_covariance[1];
      cur_odom_.pose.covariance[2] = cur_ins_cov_.position_covariance[2];
      cur_odom_.pose.covariance[6] = cur_ins_cov_.position_covariance[3];
      cur_odom_.pose.covariance[7] = cur_ins_cov_.position_covariance[4];
      cur_odom_.pose.covariance[8] = cur_ins_cov_.position_covariance[5];
      cur_odom_.pose.covariance[12] = cur_ins_cov_.position_covariance[6];
      cur_odom_.pose.covariance[13] = cur_ins_cov_.position_covariance[7];
      cur_odom_.pose.covariance[14] = cur_ins_cov_.position_covariance[8];

      cur_odom_.pose.covariance[21] = cur_ins_cov_.attitude_covariance[0]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[22] = cur_ins_cov_.attitude_covariance[1]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[23] = cur_ins_cov_.attitude_covariance[2]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[27] = cur_ins_cov_.attitude_covariance[3]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[28] = cur_ins_cov_.attitude_covariance[4]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[29] = cur_ins_cov_.attitude_covariance[5]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[33] = cur_ins_cov_.attitude_covariance[6]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[34] = cur_ins_cov_.attitude_covariance[7]*degrees_square_to_radians_square;
      cur_odom_.pose.covariance[35] = cur_ins_cov_.attitude_covariance[8]*degrees_square_to_radians_square;

      cur_odom_.twist.covariance[0] = cur_ins_cov_.velocity_covariance[0];
      cur_odom_.twist.covariance[1] = cur_ins_cov_.velocity_covariance[1];
      cur_odom_.twist.covariance[2] = cur_ins_cov_.velocity_covariance[2];
      cur_odom_.twist.covariance[6] = cur_ins_cov_.velocity_covariance[3];
      cur_odom_.twist.covariance[7] = cur_ins_cov_.velocity_covariance[4];
      cur_odom_.twist.covariance[8] = cur_ins_cov_.velocity_covariance[5];
      cur_odom_.twist.covariance[12] = cur_ins_cov_.velocity_covariance[6];
      cur_odom_.twist.covariance[13] = cur_ins_cov_.velocity_covariance[7];
      cur_odom_.twist.covariance[14] = cur_ins_cov_.velocity_covariance[8];

    }

    odom_publisher_.publish(cur_odom_);


  }

  void RawImuHandler(RawImuShort &imu, double &timestamp) {

  }

  void InsCovHandler(InsCovarianceShort &cov, double &timestamp) {
    cur_ins_cov_ = cov;
  }

  void HardwareStatusHandler(ReceiverHardwareStatus &status, double &timestamp) {

  }

  void EphemerisHandler(GpsEphemeris &ephem, double &timestamp) {
    // ROS_DEBUG("Received GpsEphemeris");

    cur_ephem_.header.stamp = ros::Time::now();
    cur_ephem_.gps_time = timestamp;
    
    uint8_t n = ephem.prn-1;
    cur_ephem_.health[n] = ephem.health;
    cur_ephem_.semimajor_axis[n] = ephem.semi_major_axis;
    cur_ephem_.mean_anomaly[n] = ephem.anomoly_reference_time;
    cur_ephem_.eccentricity[n] = ephem.eccentricity;
    cur_ephem_.perigee_arg[n] = ephem.omega;
    cur_ephem_.cos_latitude[n] = ephem.latitude_cosine;
    cur_ephem_.sin_latitude[n] = ephem.latitude_sine;
    cur_ephem_.cos_orbit_radius[n] = ephem.orbit_radius_cosine;
    cur_ephem_.sin_orbit_radius[n] = ephem.orbit_radius_sine;
    cur_ephem_.cos_inclination[n] = ephem.inclination_cosine;
    cur_ephem_.sin_inclination[n] = ephem.inclination_sine;
    cur_ephem_.inclination_angle[n] = ephem.inclination_angle;
    cur_ephem_.right_ascension[n] = ephem.right_ascension;
    cur_ephem_.mean_motion_diff[n] = ephem.mean_motion_difference;
    cur_ephem_.inclination_rate[n] = ephem.inclination_angle_rate;
    cur_ephem_.ascension_rate[n] = ephem.right_ascension_rate;
    cur_ephem_.time_of_week[n] = ephem.time_of_week;
    cur_ephem_.reference_time[n] = ephem.time_of_ephemeris;
    cur_ephem_.clock_correction[n] = ephem.sv_clock_correction;
    cur_ephem_.group_delay[n] = ephem.group_delay_difference;
    cur_ephem_.clock_aging_1[n] = ephem.clock_aligning_param_0;
    cur_ephem_.clock_aging_2[n] = ephem.clock_aligning_param_1;
    cur_ephem_.clock_aging_3[n] = ephem.clock_aligning_param_2;

    ephemeris_publisher_.publish(cur_ephem_);
  }

  void RangeHandler(RangeMeasurements &range, double &timestamp) {
    ROS_DEBUG("Received RangeMeasurements");

    gps_msgs::DualBandRange cur_range_;
    cur_range_.header.stamp = ros::Time::now();
    cur_range_.gps_time = timestamp;
    // ROS_INFO_STREAM("NUM SATS: " << range.number_of_observations);

    for (int n=0; n!=(MAX_CHAN); ++n) {
      cur_range_.L1.prn[n] = range.range_data[n].satellite_prn;
      cur_range_.L1.psr[n] = range.range_data[n].pseudorange;
      cur_range_.L1.psr_std[n] = range.range_data[n].pseudorange_standard_deviation;
      cur_range_.L1.carrier.doppler[n] = range.range_data[n].doppler;
      cur_range_.L1.carrier.noise[n] = range.range_data[n].carrier_to_noise;
      cur_range_.L1.carrier.phase[n] = -range.range_data[n].accumulated_doppler;
      cur_range_.L1.carrier.phase_std[n] = -range.range_data[n].accumulated_doppler_std_deviation;
    }

    dual_band_range_publisher_.publish(cur_range_);
  }

  void run() {

    if (!this->getParameters())
      return;

    this->odom_publisher_ = nh_.advertise<nav_msgs::Odometry>(odom_topic_,0);
    this->nav_sat_fix_publisher_ = nh_.advertise<sensor_msgs::NavSatFix>(nav_sat_fix_topic_,0);
    this->ephemeris_publisher_ = nh_.advertise<gps_msgs::Ephemeris>(ephemeris_topic_,0);
    this->dual_band_range_publisher_ = nh_.advertise<gps_msgs::DualBandRange>(dual_band_range_topic_,0);

    //em_.setDataCallback(boost::bind(&EM61Node::HandleEmData, this, _1));
    gps_.Connect(port_,baudrate_);

    // configure default log sets
    if (gps_default_logs_period_>0) {
      // request default set of gps logs at given rate
      // convert rate to string
      ROS_INFO("Requesting default GPS messages: BESTUTMB, BESTVELB");
      std::stringstream default_logs;
      default_logs.precision(2);
      default_logs << "BESTUTMB ONTIME " << std::fixed << gps_default_logs_period_ << ";";
      default_logs << "BESTVELB ONTIME " << std::fixed << gps_default_logs_period_ << ";";
      gps_.ConfigureLogs(default_logs.str());
    }

    if (span_default_logs_period_>0) {
      ROS_INFO("Requesting default SPAN messages: INSPVAB, INSCOVB");
      // request default set of gps logs at given rate
      // convert rate to string
      std::stringstream default_logs;
      default_logs.precision(2);
      default_logs << "INSPVAB ONTIME " << std::fixed << gps_default_logs_period_ << ";";
      default_logs << "INSCOVB ONTIME " << std::fixed << gps_default_logs_period_;
      gps_.ConfigureLogs(default_logs.str());
    }

    // configure logging of ephemeris
    if (ephem_default_logs_period_>0) {
      std::stringstream default_logs;
      default_logs.precision(2);
      default_logs << "GPSEPHEMB ONTIME "  << std::fixed << ephem_default_logs_period_ << ";";
      gps_.ConfigureLogs(default_logs.str());
    }

    if (range_default_logs_period_>0) {
      std::stringstream default_logs;
      default_logs.precision(2);
      default_logs << "RANGEB ONTIME " << std::fixed << range_default_logs_period_ << ";";
      gps_.ConfigureLogs(default_logs.str());
    }

    // configure additional logs
    gps_.ConfigureLogs(log_commands_);

    // configure serial port (for rtk generally)
    if (configure_port_!="") {
      // string should contain com_port,baud_rate,rx_mode,tx_mode
      // parse message body by tokening on ","
      typedef boost::tokenizer<boost::char_separator<char> >
        tokenizer;
      boost::char_separator<char> sep(",");
      tokenizer tokens(configure_port_, sep);
      // set up iterator to go through token list
      tokenizer::iterator current_token=tokens.begin();
      std::string num_comps_string=*(current_token);
      int number_components=atoi(num_comps_string.c_str());
      // make sure the correct number of tokens were found
      int token_count=0;
      for(current_token=tokens.begin(); current_token!=tokens.end();++current_token)
      {
        token_count++;
      }

      if (token_count!=4) {
        ROS_ERROR_STREAM("Incorrect number of tokens in configure port parameter: " << configure_port_);
      } else {
        current_token=tokens.begin();
        std::string com_port = *(current_token++);
        int baudrate = atoi((current_token++)->c_str());
        std::string rx_mode = *(current_token++);
        std::string tx_mode = *(current_token++);

        ROS_INFO_STREAM("Configure com port baud rate and interface mode for " << com_port << ".");
        gps_.ConfigureInterfaceMode(com_port,rx_mode,tx_mode);
        gps_.ConfigureBaudRate(com_port,baudrate);
      }
    }
    ros::spin();
  } // function

protected:

  void disconnect() {
    //em_.stopReading();
    //em_.disconnect();
  }

  bool getParameters() {
    // Get the serial ports

    nh_.param("odom_topic", odom_topic_, std::string("/gps_odom"));
    ROS_INFO_STREAM("Odom Topic: " << odom_topic_);

    nh_.param("nav_sat_fix_topic", nav_sat_fix_topic_, std::string("/gps_fix"));
    ROS_INFO_STREAM("NavSatFix Topic: " << nav_sat_fix_topic_);

    nh_.param("ephemeris_topic", ephemeris_topic_, std::string("/ephemeris"));
    ROS_INFO_STREAM("Ephemeris Topic: " << ephemeris_topic_);

    nh_.param("dual_band_range_topic", dual_band_range_topic_, std::string("/range"));
    ROS_INFO_STREAM("DualBandRange Topic: " << dual_band_range_topic_);

    nh_.param("port", port_, std::string("/dev/ttyUSB0"));
    ROS_INFO_STREAM("Port: " << port_);

    nh_.param("baudrate", baudrate_, 9600);
    ROS_INFO_STREAM("Baudrate: " << baudrate_);

    nh_.param("log_commands", log_commands_, std::string("BESTUTMB ONTIME 1.0"));
    ROS_INFO_STREAM("Log Commands: " << log_commands_);

    nh_.param("configure_port", configure_port_, std::string(""));
    ROS_INFO_STREAM("Configure port: " << configure_port_);

    nh_.param("gps_default_logs_period", gps_default_logs_period_, 0.05);
    ROS_INFO_STREAM("Default GPS logs period: " << gps_default_logs_period_);

    nh_.param("span_default_logs_period", span_default_logs_period_, 0.05);
    ROS_INFO_STREAM("Default SPAN logs period: " << span_default_logs_period_);

    nh_.param("ephem_default_logs_period", ephem_default_logs_period_, 60.0);
    ROS_INFO_STREAM("Default Ephemeris logs period: " << ephem_default_logs_period_);

    nh_.param("range_default_logs_period", range_default_logs_period_, 0.05);
    ROS_INFO_STREAM("Default Range logs period: " << range_default_logs_period_);

    return true;
  }

  ////////////////////////////////////////////////////////////////
  // ROSNODE Members
  ////////////////////////////////////////////////////////////////
  ros::NodeHandle nh_;
  ros::Publisher odom_publisher_;
  ros::Publisher nav_sat_fix_publisher_;
  ros::Publisher ephemeris_publisher_;
  ros::Publisher dual_band_range_publisher_;

  Novatel gps_;
  std::string odom_topic_;
  std::string nav_sat_fix_topic_;
  std::string ephemeris_topic_;
  std::string dual_band_range_topic_;
  std::string port_;
  std::string log_commands_;
  std::string configure_port_;
  double gps_default_logs_period_;
  double span_default_logs_period_;
  double ephem_default_logs_period_;
  double range_default_logs_period_;
  int baudrate_;
  double poll_rate_;

  Velocity cur_velocity_;
  InsCovarianceShort cur_ins_cov_;
  gps_msgs::Ephemeris cur_ephem_;


};

int main(int argc, char **argv) {
  ros::init(argc, argv, "novatel_node");
  
  NovatelNode node;
  
  node.run();
  
  return 0;
}
