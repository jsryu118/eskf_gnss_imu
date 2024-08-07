#include <eigen_conversions/eigen_msg.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/NavSatFix.h>

#include <Eigen/Core>
#include <deque>
#include <fstream>
#include <iostream>
#include <tf/transform_broadcaster.h>
#include "imu_gnss_fusion/kf.h"

namespace ryu {

class FusionNode {
   public:
    FusionNode(ros::NodeHandle &nh) {
        double acc_n, gyr_n, acc_w, gyr_w;
        double gnss_x_imuFrame, gnss_y_imuFrame, gnss_z_imuFrame;
        double local_long, local_lat, local_alt;
        
        nh.param("acc_noise", acc_n, 1e-2);
        nh.param("gyr_noise", gyr_n, 1e-4);
        nh.param("acc_bias_noise", acc_w, 1e-6);
        nh.param("gyr_bias_noise", gyr_w, 1e-8);

        nh.param("gnss_x_imuFrame", gnss_x_imuFrame, 0.);
        nh.param("gnss_y_imuFrame", gnss_y_imuFrame, 0.);
        nh.param("gnss_z_imuFrame", gnss_z_imuFrame, 0.);

        nh.param("acc_std_threshold", acc_std_threshold, 3.);
        nh.param("pub_tf", pub_tf, false);
        nh.param("viz_path", viz_path, false);

        // default : UNIST
        nh.param("local_long", local_long, 1e-4);
        nh.param("local_lat", local_lat, 1e-6);
        nh.param("local_alt", local_alt, 1e-8);

        nh.param<std::string>("frame_id", frame_id, "map");
        nh.param<std::string>("child_frame_id", child_frame_id, "gnss_frame");
        std::string topic_imu, topic_gnss;
        nh.param<std::string>("topic_imu", topic_imu, "/imu/data");
        nh.param<std::string>("topic_gnss", topic_gnss, "/gnss_1/llh_position");
        init_lla_ = Eigen::Vector3d(local_long, local_lat, local_alt);

        kf_ptr_ = std::make_unique<KF>(acc_n, gyr_n, acc_w, gyr_w);

        // gnss position from imu frame
        // This is constant value
        I_p_gnss_ = Eigen::Vector3d(gnss_x_imuFrame, gnss_y_imuFrame, gnss_z_imuFrame);

        // ROS sub & pub
        imu_sub_ = nh.subscribe(topic_imu, 10, &FusionNode::imu_callback, this);
        gnss_sub_ = nh.subscribe(topic_gnss, 10, &FusionNode::gnss_callback, this);

        path_pub_ = nh.advertise<nav_msgs::Path>("nav_path", 10);
        odom_pub_ = nh.advertise<nav_msgs::Odometry>("nav_odom", 10);

        // log files
        // file_gnss_.open("fusion_gnss.csv");
        // file_state_.open("fusion_state.csv");
    }

    ~FusionNode() {
        // file_gnss_.close();
        // file_state_.close();
    }

    void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg);
    void gnss_callback(const sensor_msgs::NavSatFixConstPtr &gnss_msg);

    bool init_rot_from_imudata(Eigen::Matrix3d &r_GI);

    void publish_save_state();

   private:
    ros::Subscriber imu_sub_;
    ros::Subscriber gnss_sub_;
    ros::Publisher path_pub_;
    ros::Publisher odom_pub_;
    tf::TransformBroadcaster tf_broadcaster_;

    nav_msgs::Path nav_path_;

    // rosparam
    std::string frame_id;
    std::string child_frame_id;
    bool pub_tf;
    double acc_std_threshold;
    bool viz_path;

    // init
    bool initialized_rot_ = false;
    bool initialized_gnss_ = false;
    const int kImuBufSize = 100;
    std::deque<ImuDataConstPtr> imu_buf_;
    ImuDataConstPtr last_imu_ptr_;
    Eigen::Vector3d init_lla_;

    Eigen::Vector3d I_p_gnss_;


    // KF
    KFPtr kf_ptr_;

    // log files
    // std::ofstream file_gnss_;
    // std::ofstream file_state_;
};

void FusionNode::imu_callback(const sensor_msgs::ImuConstPtr &imu_msg) {
    ImuDataPtr imu_data_ptr = std::make_shared<ImuData>();
    imu_data_ptr->timestamp = imu_msg->header.stamp.toSec();
    imu_data_ptr->acc[0] = imu_msg->linear_acceleration.x;
    imu_data_ptr->acc[1] = imu_msg->linear_acceleration.y;
    imu_data_ptr->acc[2] = imu_msg->linear_acceleration.z;
    imu_data_ptr->gyr[0] = imu_msg->angular_velocity.x;
    imu_data_ptr->gyr[1] = imu_msg->angular_velocity.y;
    imu_data_ptr->gyr[2] = imu_msg->angular_velocity.z;

    if (!initialized_gnss_) {
        imu_buf_.push_back(imu_data_ptr);
        if (imu_buf_.size() > kImuBufSize) imu_buf_.pop_front();
        return;
    }

    kf_ptr_->predict(last_imu_ptr_, imu_data_ptr);

    last_imu_ptr_ = imu_data_ptr;

    publish_save_state();
}

void FusionNode::gnss_callback(const sensor_msgs::NavSatFixConstPtr &gnss_msg) {
    // status 2:ground based fix
    // status 0:no fix
    // status 1:satellite based fix
    if (gnss_msg->status.status != 2) {
        printf("[gnss_imu %s] ERROR: Bad gnss Message!!!\n", __FUNCTION__);
        return;
    }

    gnssDataPtr gnss_data_ptr = std::make_shared<gnssData>();
    gnss_data_ptr->timestamp = gnss_msg->header.stamp.toSec();
    gnss_data_ptr->lla[0] = gnss_msg->latitude;
    gnss_data_ptr->lla[1] = gnss_msg->longitude;
    gnss_data_ptr->lla[2] = gnss_msg->altitude;
    gnss_data_ptr->cov = Eigen::Map<const Eigen::Matrix3d>(gnss_msg->position_covariance.data());

    if (!initialized_rot_) {
        if (imu_buf_.size() < kImuBufSize) {
            printf("[gnss_imu %s] ERROR: Not Enough IMU data for Initialization!!!\n", __FUNCTION__);
            return;
        }

        last_imu_ptr_ = imu_buf_.back();
        if (std::abs(gnss_data_ptr->timestamp - last_imu_ptr_->timestamp) > 0.5) {
            printf("[gnss_imu %s] ERROR: gnss and Imu timestamps are not synchronized!!!\n", __FUNCTION__);
            return;
        }

        kf_ptr_->state_ptr_->timestamp = last_imu_ptr_->timestamp;

        if (!init_rot_from_imudata(kf_ptr_->state_ptr_->r_GI)) return;

        // init_lla_ = gnss_data_ptr->lla;

        initialized_rot_ = true;

        printf("[gnss_imu %s] System initialized.\n", __FUNCTION__);

        return;
    }

    // convert WGS84 to ENU frame
    Eigen::Vector3d p_G_gnss; // position of gnss from global(map) frame
    ryu::lla2enu(init_lla_, gnss_data_ptr->lla, &p_G_gnss);

    const Eigen::Vector3d &p_GI = kf_ptr_->state_ptr_->p_GI;
    const Eigen::Matrix3d &r_GI = kf_ptr_->state_ptr_->r_GI;

    // residual
    Eigen::Vector3d residual = p_G_gnss - (p_GI + r_GI * I_p_gnss_);
    double abs_residual = residual.norm();
    // jacobian
    Eigen::Matrix<double, 3, 15> H;
    H.setZero();
    H.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
    H.block<3, 3>(0, 6) = -r_GI * ryu::skew_matrix(I_p_gnss_);

    // measurement covariance
    const Eigen::Matrix3d &V = gnss_data_ptr->cov;

    kf_ptr_->update_measurement(H, V, residual);

    if (!initialized_gnss_)  initialized_gnss_ = true;

    // save gnss lla
    // file_gnss_ << std::fixed << std::setprecision(15)
    //           << gnss_data_ptr->timestamp << ", "
    //           << gnss_data_ptr->lla[0] << ", " << gnss_data_ptr->lla[1] << ", " << gnss_data_ptr->lla[2]
    //           << std::endl;
}

bool FusionNode::init_rot_from_imudata(Eigen::Matrix3d &r_GI) {
    // mean and std of IMU accs
    Eigen::Vector3d sum_acc(0., 0., 0.);
    for (const auto imu_data : imu_buf_) {
        sum_acc += imu_data->acc;
    }
    const Eigen::Vector3d mean_acc = sum_acc / (double)imu_buf_.size();
    printf("[gnss_imu %s] mean_acc: (%f, %f, %f)!!!\n", __FUNCTION__, mean_acc[0], mean_acc[1], mean_acc[2]);

    Eigen::Vector3d sum_err2(0., 0., 0.);
    for (const auto imu_data : imu_buf_) sum_err2 += (imu_data->acc - mean_acc).cwiseAbs2();
    const Eigen::Vector3d std_acc = (sum_err2 / (double)imu_buf_.size()).cwiseSqrt();

    // acc std limit: 3
    if (std_acc.maxCoeff() > acc_std_threshold) {
        printf("[gnss_imu %s] Too big acc std: (%f, %f, %f)!!!\n", __FUNCTION__, std_acc[0], std_acc[1], std_acc[2]);
        printf("[gnss_imu %s] please stop !!!!!!\n", __FUNCTION__);
        printf("[gnss_imu %s] please stop !!!!!!\n", __FUNCTION__);
        printf("[gnss_imu %s] please stop !!!!!!\n", __FUNCTION__);
        return false;
    }

    // Compute rotation.
    // ref: https://github.com/rpng/open_vins/blob/master/ov_core/src/init/InertialInitializer.cpp

    // Three axises of the ENU frame in the IMU frame.
    // z-axis
    const Eigen::Vector3d &z_axis = mean_acc.normalized();

    // x-axis
    Eigen::Vector3d x_axis = Eigen::Vector3d::UnitX() - z_axis * z_axis.transpose() * Eigen::Vector3d::UnitX();
    x_axis.normalize();

    // y-axis
    Eigen::Vector3d y_axis = z_axis.cross(x_axis);
    y_axis.normalize();

    Eigen::Matrix3d r_IG;
    r_IG.block<3, 1>(0, 0) = x_axis;
    r_IG.block<3, 1>(0, 1) = y_axis;
    r_IG.block<3, 1>(0, 2) = z_axis;

    r_GI = r_IG.transpose();

    return true;
}

void FusionNode::publish_save_state() {
    // publish the odometry

    nav_msgs::Odometry odom_msg;
    odom_msg.header.frame_id = frame_id;
    odom_msg.header.stamp = ros::Time::now();
    Eigen::Isometry3d T_wb = Eigen::Isometry3d::Identity();
    T_wb.linear() = kf_ptr_->state_ptr_->r_GI;
    T_wb.translation() = kf_ptr_->state_ptr_->p_GI;
    tf::poseEigenToMsg(T_wb, odom_msg.pose.pose);
    tf::vectorEigenToMsg(kf_ptr_->state_ptr_->v_GI, odom_msg.twist.twist.linear);
    Eigen::Matrix3d P_pp = kf_ptr_->state_ptr_->cov.block<3, 3>(0, 0);
    Eigen::Matrix3d P_po = kf_ptr_->state_ptr_->cov.block<3, 3>(0, 6);
    Eigen::Matrix3d P_op = kf_ptr_->state_ptr_->cov.block<3, 3>(6, 0);
    Eigen::Matrix3d P_oo = kf_ptr_->state_ptr_->cov.block<3, 3>(6, 6);
    Eigen::Matrix<double, 6, 6, Eigen::RowMajor> P_imu_pose = Eigen::Matrix<double, 6, 6>::Zero();
    P_imu_pose << P_pp, P_po, P_op, P_oo;
    for (int i = 0; i < 36; i++)
        odom_msg.pose.covariance[i] = P_imu_pose.data()[i];
    odom_pub_.publish(odom_msg);

    // broadcast tf
    if (pub_tf){
        tf::Transform tf_transform;
        tf_transform.setOrigin(tf::Vector3(T_wb.translation().x(), T_wb.translation().y(), T_wb.translation().z()));
        tf::Quaternion tf_quat;
        tf::Matrix3x3 rot_matrix;
        rot_matrix.setValue(
            T_wb(0, 0), T_wb(0, 1), T_wb(0, 2),
            T_wb(1, 0), T_wb(1, 1), T_wb(1, 2),
            T_wb(2, 0), T_wb(2, 1), T_wb(2, 2)
        );
        rot_matrix.getRotation(tf_quat);
        tf_transform.setRotation(tf_quat);
        tf_broadcaster_.sendTransform(tf::StampedTransform(tf_transform, odom_msg.header.stamp, frame_id, child_frame_id));
    }

    // publish the path
    if (viz_path){
        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.header = odom_msg.header;
        pose_stamped.pose = odom_msg.pose.pose;
        nav_path_.header = pose_stamped.header;
        nav_path_.poses.push_back(pose_stamped);
        path_pub_.publish(nav_path_);
    }

    // save state p q lla
    // std::shared_ptr<KF::State> kf_state(kf_ptr_->state_ptr_);
    // Eigen::Vector3d lla;
    // ryu::enu2lla(init_lla_, kf_state->p_GI, &lla);  // convert ENU state to lla
    // const Eigen::Quaterniond q_GI(kf_state->r_GI);
    // file_state_ << std::fixed << std::setprecision(15)
    //             << kf_state->timestamp << ", "
    //             << kf_state->p_GI[0] << ", " << kf_state->p_GI[1] << ", " << kf_state->p_GI[2] << ", "
    //             << q_GI.x() << ", " << q_GI.y() << ", " << q_GI.z() << ", " << q_GI.w() << ", "
    //             << lla[0] << ", " << lla[1] << ", " << lla[2]
    //             << std::endl;
}

}  // namespace ryu

int main(int argc, char **argv) {
    ros::init(argc, argv, "single_gnss_imu_eskf_node");

    ros::NodeHandle nh("~");
    ryu::FusionNode fusion_node(nh);

    ros::spin();

    return 0;
}
