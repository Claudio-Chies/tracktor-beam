#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include <opencv2/aruco.hpp>
#include <px4_msgs/msg/distance_sensor.hpp>
#include <px4_msgs/msg/landing_target_pose.hpp>

class ArucoTrackerNode : public rclcpp::Node
{
public:
	ArucoTrackerNode();

private:
	void image_callback(const sensor_msgs::msg::Image::SharedPtr msg);
	void distance_sensor_callback(const px4_msgs::msg::DistanceSensor::SharedPtr msg);

	rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr _image_sub;
	rclcpp::Subscription<px4_msgs::msg::DistanceSensor>::SharedPtr _distance_sub;
	rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr _image_pub;
	cv::Ptr<cv::aruco::Dictionary> _dictionary;
	// float _ground_distance = 0.f;
	float _ground_distance = 1.f;
	cv::Mat _camera_matrix;
	cv::Mat _dist_coeffs;
};

ArucoTrackerNode::ArucoTrackerNode()
	: Node("aruco_tracker_node")
{
	RCLCPP_INFO(this->get_logger(), "ArucoTrackerNode :)");
	// Define aruco tag dictionary to use
	_dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_250);

	// Load camera calibration data from YAML file
	cv::FileStorage fs("usb_cam_calib.yml", cv::FileStorage::READ);
	if (fs.isOpened()) {
		fs["camera_matrix"] >> _camera_matrix;
		fs["distortion_coefficients"] >> _dist_coeffs;
		fs.release();
	} else {
		RCLCPP_ERROR(this->get_logger(), "Failed to open camera calibration file");
	}

	if (_camera_matrix.empty() || _dist_coeffs.empty()) {
		RCLCPP_ERROR(this->get_logger(), "Failed to load camera parameters correctly.");
	}

	// Ensure matrix is of type double
	_camera_matrix.convertTo(_camera_matrix, CV_64F);

	// Setup publishers and subscribers
	auto qos = rclcpp::QoS(rclcpp::QoSInitialization(RMW_QOS_POLICY_HISTORY_KEEP_LAST, 5));
	qos.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);

	_image_sub = this->create_subscription<sensor_msgs::msg::Image>(
		"/image_raw", qos, std::bind(&ArucoTrackerNode::image_callback, this, std::placeholders::_1));
	_distance_sub = this->create_subscription<px4_msgs::msg::DistanceSensor>(
		"/fmu/out/distance_sensor", qos, std::bind(&ArucoTrackerNode::distance_sensor_callback, this, std::placeholders::_1));
	_image_pub = this->create_publisher<sensor_msgs::msg::Image>(
		"/image_proc", qos);
}

void ArucoTrackerNode::distance_sensor_callback(const px4_msgs::msg::DistanceSensor::SharedPtr msg)
{
	_ground_distance = msg->current_distance;
}

void ArucoTrackerNode::image_callback(const sensor_msgs::msg::Image::SharedPtr msg)
{
	try {
		cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);

		// Calculate poses
		std::vector<int> ids;
		std::vector<std::vector<cv::Point2f>> corners;
		std::vector<cv::Vec3d> rvecs, tvecs;

		cv::aruco::detectMarkers(cv_ptr->image, _dictionary, corners, ids);

		// Array of arrays where each array is a set of marker IDs
		if (ids.size() > 0) {

			RCLCPP_INFO(this->get_logger(), "drawing detected markers");
			cv::aruco::drawDetectedMarkers(cv_ptr->image, corners, ids);

			for (size_t i = 0; i < ids.size(); i++) {
				float pixel_width = cv::norm(corners[i][0] - corners[i][1]);
				float marker_size = (pixel_width / _camera_matrix.at<double>(0, 0)) * _ground_distance;

				if (!std::isnan(marker_size) && !std::isinf(marker_size)) {
					RCLCPP_INFO(this->get_logger(), "marker_size %f", marker_size);
					std::vector<cv::Point3f> objectPoints;
					float halfSize = marker_size / 2.0f;
					objectPoints.push_back(cv::Point3f(-halfSize,  halfSize, 0));  // top left
					objectPoints.push_back(cv::Point3f( halfSize,  halfSize, 0));  // top right
					objectPoints.push_back(cv::Point3f( halfSize, -halfSize, 0));  // bottom right
					objectPoints.push_back(cv::Point3f(-halfSize, -halfSize, 0));  // bottom left

					cv::Vec3d rvec, tvec;
					RCLCPP_INFO(this->get_logger(), "solvePnP");
					cv::solvePnP(objectPoints, corners[i], _camera_matrix, _dist_coeffs, rvec, tvec);

					RCLCPP_INFO(this->get_logger(), "drawing axis");
					cv::aruco::drawAxis(cv_ptr->image, _camera_matrix, _dist_coeffs, rvec, tvec, marker_size);
				}
			}
		}

		cv_bridge::CvImage out_msg;
		out_msg.header = msg->header;
		out_msg.encoding = sensor_msgs::image_encodings::BGR8;
		out_msg.image = cv_ptr->image;
		_image_pub->publish(*out_msg.toImageMsg().get());

	} catch (const cv_bridge::Exception& e) {
		RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
	}
}

int main(int argc, char **argv)
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<ArucoTrackerNode>());
	rclcpp::shutdown();
	return 0;
}