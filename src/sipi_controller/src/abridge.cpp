#include <ros/ros.h>

//ROS libraries
#include <tf/transform_datatypes.h>

//ROS messages
#include <std_msgs/Float32.h>
#include <std_msgs/String.h>
#include <geometry_msgs/Quaternion.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/Range.h>
#include <std_msgs/UInt8.h>

//Package include
#include "sipi_controller/usbSerial.h"

// maximum +/- limits for velocity commands
#define MAX_LIN_VEL_CMD 0.3  // m/s
#define MAX_ANG_VEL_CMD 0.5  // rad/s
// maximum safe value to write to motor controller
#define MAX_MOTOR_CMD 120

using namespace std;

//aBridge functions
// theses are the topics that need to communcate through the arduino
// give drive commands.  only listens to linear:x (m/s) and angular:z (rad/s)
void driveCommandHandler(const geometry_msgs::Twist::ConstPtr& message);
// angle in radians for finger joint
void fingerAngleHandler(const std_msgs::Float32::ConstPtr& angle);
// angle in radians for wrist joint
void wristAngleHandler(const std_msgs::Float32::ConstPtr& angle);

// timer to trigger read of serial data from arduino
void serialActivityTimer(const ros::TimerEvent& e);
// helper function to publish all of the ROS topics from arduino data
void publishRosTopics();
//parse the serial data from the arduino into data structures
void parseData(string data);

std::string getHumanFriendlyTime();

//Globals
geometry_msgs::QuaternionStamped fingerAngle;
geometry_msgs::QuaternionStamped wristAngle;
geometry_msgs::Twist cmd_vel_msg;
sensor_msgs::Imu imu;
nav_msgs::Odometry odom;
sensor_msgs::Range sonarLeft;
sensor_msgs::Range sonarCenter;
sensor_msgs::Range sonarRight;
USBSerial usb;
const int baud = 115200;
char dataCmd[] = "d\n";
char moveCmd[16];
char host[128];
const float deltaTime = 0.1; //abridge's update interval
int currentMode = 0;
string publishedName;

float heartbeat_publish_interval = 2;

//Publishers for ROS data from the Arduino
ros::Publisher fingerAnglePublish;
ros::Publisher wristAnglePublish;
ros::Publisher imuPublish;
ros::Publisher odomPublish;
ros::Publisher sonarLeftPublish;
ros::Publisher sonarCenterPublish;
ros::Publisher sonarRightPublish;
ros::Publisher infoLogPublisher;
ros::Publisher heartbeatPublisher;

//Subscribers
ros::Subscriber driveControlSubscriber;
ros::Subscriber fingerAngleSubscriber;
ros::Subscriber wristAngleSubscriber;
ros::Subscriber modeSubscriber;

//Timers
ros::Timer publishTimer;
ros::Timer publish_heartbeat_timer;

//Callback handlers
void publishHeartBeatTimerEventHandler(const ros::TimerEvent& event);
void modeHandler(const std_msgs::UInt8::ConstPtr& message);

int main(int argc, char **argv) {

  gethostname(host, sizeof (host));
  string hostname(host);
  ros::init(argc, argv, (hostname + "_ABRIDGE"));

  ros::NodeHandle param("~");
  string devicePath;
  param.param("device", devicePath, string("/dev/ttyUSB0"));
  usb.openUSBPort(devicePath, baud);

  sleep(5);

  ros::NodeHandle aNH;

  if (argc >= 2) {
    publishedName = argv[1];
    ROS_INFO_STREAM(publishedName << ": ABridge module started." << endl);
  } else {
    publishedName = hostname;
    ROS_INFO_STREAM("abridge: No Name Selected. Default is: " << 
      publishedName << endl);
  }

  fingerAnglePublish = aNH.advertise<geometry_msgs::QuaternionStamped>(
    (publishedName + "/fingerAngle/prev_cmd"), 10);
  wristAnglePublish = aNH.advertise<geometry_msgs::QuaternionStamped>(
    (publishedName + "/wristAngle/prev_cmd"), 10);
  imuPublish = aNH.advertise<sensor_msgs::Imu>((publishedName + "/imu"), 10);
  odomPublish = aNH.advertise<nav_msgs::Odometry>(
    (publishedName + "/odom"), 10);
  sonarLeftPublish = aNH.advertise<sensor_msgs::Range>(
    (publishedName + "/sonarLeft"), 10);
  sonarCenterPublish = aNH.advertise<sensor_msgs::Range>(
    (publishedName + "/sonarCenter"), 10);
  sonarRightPublish = aNH.advertise<sensor_msgs::Range>(
    (publishedName + "/sonarRight"), 10);
  infoLogPublisher = aNH.advertise<std_msgs::String>("/infoLog", 1, true);
  heartbeatPublisher = aNH.advertise<std_msgs::String>(
    (publishedName + "/abridge/heartbeat"), 1, true);

  driveControlSubscriber = aNH.subscribe(
    (publishedName + "/driveControl"), 10, driveCommandHandler);
  fingerAngleSubscriber = aNH.subscribe(
    (publishedName + "/fingerAngle/cmd"), 1, fingerAngleHandler);
  wristAngleSubscriber = aNH.subscribe(
    (publishedName + "/wristAngle/cmd"), 1, wristAngleHandler);
  modeSubscriber = aNH.subscribe(
    (publishedName + "/mode"), 1, modeHandler);


  publishTimer = aNH.createTimer(ros::Duration(deltaTime), serialActivityTimer);
  publish_heartbeat_timer = aNH.createTimer(
    ros::Duration(heartbeat_publish_interval), 
    publishHeartBeatTimerEventHandler);

  imu.header.frame_id = publishedName+"/base_link";

  odom.header.frame_id = publishedName+"/odom";
  odom.child_frame_id = publishedName+"/base_link";

  ros::spin();

  return EXIT_SUCCESS;
}
float limit(float val, float max) {
  if (val > max) {
    return max;
  } else if (val < -max) {
    return -max;
  } else {
    return val;
  } 
}

//This command handler recives a twist setpoint
void driveCommandHandler(const geometry_msgs::Twist::ConstPtr& message) {
  cmd_vel_msg = *message;
  cmd_vel_msg.linear.x = limit(cmd_vel_msg.linear.x, MAX_LIN_VEL_CMD); 
  cmd_vel_msg.angular.z = limit(cmd_vel_msg.angular.z, MAX_LIN_VEL_CMD); 
}

void calculateMotorCommands(void) {
  // Cap motor commands at 120. Experimentally determined that 
  // high values (tested 180 and 255) can cause 
  // the hardware to fail when the robot moves itself too violently.

  // limit motor commands to safe maximum value
  geometry_msgs::Twist err;
  err.linear.x = odom.twist.twist.linear.x - cmd_vel_msg.linear.x;
  err.angular.z = odom.twist.twist.angular.z - cmd_vel_msg.angular.z;
  float Kp = 10;
  float Ki = 10;
  int vx = err.linear.x * Kp;
  int vz = err.angular.z * Kp;
  
  int motor_left = vx - vz;
  int motor_right = vx + vz;

  //format data for arduino into c string
  sprintf(moveCmd, "v,%d,%d\n", motor_left, motor_right); 
  usb.sendData(moveCmd);       //send movement command to arduino over usb
}

// The finger and wrist handlers receive gripper angle commands in 
// floating point
// radians, write them to a string and send that to the arduino
// for processing.
void fingerAngleHandler(const std_msgs::Float32::ConstPtr& angle) {
  char cmd[16]={'\0'};

  // Avoid dealing with negative exponents which confuse the conversion to string by checking if the angle is small
  if (angle->data < 0.01) {
    // 'f' indicates this is a finger command to the arduino
    sprintf(cmd, "f,0\n");
  } else {
    sprintf(cmd, "f,%.4g\n", angle->data);
  }
  usb.sendData(cmd);
}

void wristAngleHandler(const std_msgs::Float32::ConstPtr& angle) {

  char cmd[16]={'\0'};

  // Avoid dealing with negative exponents which confuse the conversion to string by checking if the angle is small
  if (angle->data < 0.01) {
    // 'w' indicates this is a wrist command to the arduino
    sprintf(cmd, "w,0\n");
  } else {
    sprintf(cmd, "w,%.4g\n", angle->data);
  }
  usb.sendData(cmd);
}

void serialActivityTimer(const ros::TimerEvent& e) {
  usb.sendData(dataCmd);
  parseData(usb.readData());
  publishRosTopics();
  calculateMotorCommands();
}

void publishRosTopics() {
  fingerAnglePublish.publish(fingerAngle);
  wristAnglePublish.publish(wristAngle);
  imuPublish.publish(imu);
  odomPublish.publish(odom);
  sonarLeftPublish.publish(sonarLeft);
  sonarCenterPublish.publish(sonarCenter);
  sonarRightPublish.publish(sonarRight);
}

void parseData(string str) {
  istringstream oss(str);
  string sentence;

  while (getline(oss, sentence, '\n')) {
    istringstream wss(sentence);
    string word;

    vector<string> dataSet;
    while (getline(wss, word, ',')) {
      dataSet.push_back(word);
    }

    if (dataSet.size() >= 3 && dataSet.at(1) == "1") {

      if (dataSet.at(0) == "GRF") {
        fingerAngle.header.stamp = ros::Time::now();
        fingerAngle.quaternion = tf::createQuaternionMsgFromRollPitchYaw(
          atof(dataSet.at(2).c_str()), 0.0, 0.0);
      }
      else if (dataSet.at(0) == "GRW") {
        wristAngle.header.stamp = ros::Time::now();
        wristAngle.quaternion = tf::createQuaternionMsgFromRollPitchYaw(
          atof(dataSet.at(2).c_str()), 0.0, 0.0);
      }
      else if (dataSet.at(0) == "IMU") {
        imu.header.stamp = ros::Time::now();
        imu.linear_acceleration.x = atof(dataSet.at(2).c_str());
        imu.linear_acceleration.y = 0; //atof(dataSet.at(3).c_str());
        imu.linear_acceleration.z = atof(dataSet.at(4).c_str());
        imu.angular_velocity.x = atof(dataSet.at(5).c_str());
        imu.angular_velocity.y = atof(dataSet.at(6).c_str());
        imu.angular_velocity.z = atof(dataSet.at(7).c_str());
        imu.orientation = tf::createQuaternionMsgFromRollPitchYaw(
          atof(dataSet.at(8).c_str()), atof(dataSet.at(9).c_str()), 
          atof(dataSet.at(10).c_str()));
      }
      else if (dataSet.at(0) == "ODOM") {
        odom.header.stamp = ros::Time::now();
        odom.pose.pose.position.x += atof(dataSet.at(2).c_str()) / 100.0;
        odom.pose.pose.position.y += atof(dataSet.at(3).c_str()) / 100.0;
        odom.pose.pose.position.z = 0.0;
        odom.pose.pose.orientation = tf::createQuaternionMsgFromYaw(
          atof(dataSet.at(4).c_str()));
        odom.twist.twist.linear.x = atof(dataSet.at(5).c_str()) / 100.0;
        odom.twist.twist.linear.y = atof(dataSet.at(6).c_str()) / 100.0;
        odom.twist.twist.angular.z = atof(dataSet.at(7).c_str());
      }
      else if (dataSet.at(0) == "USL") {
        sonarLeft.header.stamp = ros::Time::now();
        sonarLeft.range = atof(dataSet.at(2).c_str()) / 100.0;
      }
      else if (dataSet.at(0) == "USC") {
        sonarCenter.header.stamp = ros::Time::now();
        sonarCenter.range = atof(dataSet.at(2).c_str()) / 100.0;
      }
      else if (dataSet.at(0) == "USR") {
        sonarRight.header.stamp = ros::Time::now();
        sonarRight.range = atof(dataSet.at(2).c_str()) / 100.0;
      }

    }
  }
}

void modeHandler(const std_msgs::UInt8::ConstPtr& message) {
  currentMode = message->data;
}

void publishHeartBeatTimerEventHandler(const ros::TimerEvent&) {
  std_msgs::String msg;
  msg.data = "";
  heartbeatPublisher.publish(msg);
}