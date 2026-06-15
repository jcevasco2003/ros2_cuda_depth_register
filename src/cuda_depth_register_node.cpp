#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <cuda_runtime.h>

#include "cuda_depth_register/common_types.hpp"
#include "cuda_depth_register/depth_register.hpp"

#include <mutex>

class CudaDepthRegisterNode : public rclcpp::Node
{
public:
    CudaDepthRegisterNode()
    : Node("cuda_depth_register")
    {
        using std::placeholders::_1;

        // ------------------------
        // SUSCRIPCIONES
        // ------------------------
        auto qos = rclcpp::SensorDataQoS();

        this->declare_parameter<std::string>("depth_input_topic", "/camera/depth/image_rect_raw");
        this->declare_parameter<std::string>("depth_output_topic", "/camera/depth_registered/image_rect");
        this->declare_parameter<std::string>("color_info_topic", "/camera/color/camera_info");
        this->declare_parameter<std::string>("depth_info_topic", "/camera/depth/camera_info");
        this->declare_parameter<std::string>("target_frame", "camera_color_optical_frame");
        this->declare_parameter<std::string>("source_frame", "camera_depth_optical_frame");

        depth_input_topic_ = this->get_parameter("depth_input_topic").as_string();
        depth_output_topic_ = this->get_parameter("depth_output_topic").as_string();
        color_info_topic_ = this->get_parameter("color_info_topic").as_string();
        depth_info_topic_ = this->get_parameter("depth_info_topic").as_string();
        target_frame_ = this->get_parameter("target_frame").as_string();
        source_frame_ = this->get_parameter("source_frame").as_string();

        depth_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
            depth_input_topic_,
            qos,
            std::bind(&CudaDepthRegisterNode::depthCallback, this, _1));

        color_info_sub_ = this->create_subscription<sensor_msgs::msg::CameraInfo>(
            color_info_topic_, 10,
            std::bind(&CudaDepthRegisterNode::colorInfoCallback, this, _1));

        depth_info_sub_ = this->create_subscription<sensor_msgs::msg::CameraInfo>(
            depth_info_topic_, 10,
            std::bind(&CudaDepthRegisterNode::depthInfoCallback, this, _1));

        tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
        tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

        // ------------------------
        // PUBLISHER
        // ------------------------

        depth_pub_ = this->create_publisher<sensor_msgs::msg::Image>(
            depth_output_topic_, 10);
    }

    ~CudaDepthRegisterNode()
    {
        if (d_depth_in_) cudaFree(d_depth_in_);
        if (d_depth_out_) cudaFree(d_depth_out_);
        if (d_z_buffer_) cudaFree(d_z_buffer_);
        if (d_valid_in_) cudaFree(d_valid_in_);
        if (d_valid_out_) cudaFree(d_valid_out_);
    }

private:
    // ------------------------
    // BUFFERS DE GPU
    // ------------------------
    uint16_t* d_depth_in_ = nullptr;
    uint16_t* d_depth_out_ = nullptr;
    unsigned int* d_z_buffer_ = nullptr;

    uint8_t* d_valid_in_ = nullptr; // Para hole filling
    uint8_t* d_valid_out_ = nullptr; // Para hole filling

    int allocated_depth_pixels_ = 0;
    int allocated_color_pixels_ = 0;

    void ensureBuffers(int depth_pixels, int color_pixels)
    {
        if (depth_pixels != allocated_depth_pixels_) {
            if (d_depth_in_) cudaFree(d_depth_in_);
            cudaMalloc(&d_depth_in_, depth_pixels * sizeof(uint16_t));
            allocated_depth_pixels_ = depth_pixels;
        }

        if (color_pixels != allocated_color_pixels_) {
            if (d_depth_out_) cudaFree(d_depth_out_);
            if (d_z_buffer_) cudaFree(d_z_buffer_);
            if (d_valid_in_) cudaFree(d_valid_in_);
            if (d_valid_out_) cudaFree(d_valid_out_);

            cudaMalloc(&d_depth_out_, color_pixels * sizeof(uint16_t));
            cudaMalloc(&d_z_buffer_, color_pixels * sizeof(unsigned int));
            cudaMalloc(&d_valid_in_, color_pixels * sizeof(uint8_t));
            cudaMalloc(&d_valid_out_, color_pixels * sizeof(uint8_t));

            depth_out_.resize(color_pixels);

            allocated_color_pixels_ = color_pixels;
        }
    }

    int color_w_ = 0, color_h_ = 0;
    int depth_w_ = 0, depth_h_ = 0;
    Extrinsics T_color_depth_; // Pasa de depth a color
    Intrinsics color_K_;
    Intrinsics depth_K_;
    std::string depth_input_topic_;
    std::string depth_output_topic_;
    std::string color_info_topic_;
    std::string depth_info_topic_;
    std::string target_frame_;
    std::string source_frame_;
    bool has_color_info_ = false;
    bool has_depth_info_ = false;
    bool has_extrinsics_ = false;
    bool constants_updated_ = false;
    // ------------------------
    // CALLBACKS DE CAMERA INFO
    // ------------------------

    void colorInfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg)
    {
        color_K_.fx = msg->k[0];
        color_K_.fy = msg->k[4];
        color_K_.cx = msg->k[2];
        color_K_.cy = msg->k[5];
        color_K_.inv_fx = (msg->k[0] != 0.0f) ? 1.0f / msg->k[0] : 0.0f;
        color_K_.inv_fy = (msg->k[4] != 0.0f) ? 1.0f / msg->k[4] : 0.0f;
        color_w_ = msg->width;
        color_h_ = msg->height;
        
        if (!has_color_info_)
        {
        RCLCPP_INFO_THROTTLE(this->get_logger(), *this->get_clock(), 2000,
            "Color intrinsics loaded: fx=%f fy=%f cx=%f cy=%f",
            color_K_.fx, color_K_.fy, color_K_.cx, color_K_.cy);
        }
        has_color_info_ = true;
    }

    void depthInfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg)
    {
        depth_K_.fx = msg->k[0];
        depth_K_.fy = msg->k[4];
        depth_K_.cx = msg->k[2];
        depth_K_.cy = msg->k[5];
        depth_K_.inv_fx = (msg->k[0] != 0.0f) ? 1.0f / msg->k[0] : 0.0f;
        depth_K_.inv_fy = (msg->k[4] != 0.0f) ? 1.0f / msg->k[4] : 0.0f;
        //depth_w_ = msg->width;
        //depth_h_ = msg->height;
        
        if (!has_depth_info_)
        {
        RCLCPP_INFO_THROTTLE(this->get_logger(), *this->get_clock(), 2000,
            "Depth intrinsics loaded: fx=%f fy=%f cx=%f cy=%f",
            depth_K_.fx, depth_K_.fy, depth_K_.cx, depth_K_.cy);
        }
        has_depth_info_ = true;
    }

    // ------------------------
    // CALLBACK DE DEPTH
    // ------------------------
    std::vector<uint16_t> depth_out_; // Buffer para evitar realloc

    void depthCallback(const sensor_msgs::msg::Image::SharedPtr msg)
    {
        /*
        RCLCPP_INFO_THROTTLE(this->get_logger(), *this->get_clock(), 2000,
            "Received depth frame: width=%d height=%d encoding=%s data_size=%zu",
            msg->width, msg->height, msg->encoding.c_str(), msg->data.size());
        */
        geometry_msgs::msg::TransformStamped transform;

        try
        {
            transform = tf_buffer_->lookupTransform(
                target_frame_,
                source_frame_,
                tf2::TimePointZero
            );
        }
        catch (tf2::TransformException &ex)
        {
            RCLCPP_WARN(this->get_logger(), "TF no disponible: %s", ex.what());
            return;
        }

        tf2::Quaternion q;
        tf2::fromMsg(transform.transform.rotation, q);

        tf2::Matrix3x3 R(q);

        T_color_depth_.R[0] = R[0][0]; T_color_depth_.R[1] = R[0][1]; T_color_depth_.R[2] = R[0][2];
        T_color_depth_.R[3] = R[1][0]; T_color_depth_.R[4] = R[1][1]; T_color_depth_.R[5] = R[1][2];
        T_color_depth_.R[6] = R[2][0]; T_color_depth_.R[7] = R[2][1]; T_color_depth_.R[8] = R[2][2];

        T_color_depth_.t[0] = transform.transform.translation.x;
        T_color_depth_.t[1] = transform.transform.translation.y;
        T_color_depth_.t[2] = transform.transform.translation.z;

        if (!has_extrinsics_)
        {
            RCLCPP_INFO(this->get_logger(), "Extrinsics loaded (T_color_depth) at first frame: R=[%.3f %.3f %.3f; %.3f %.3f %.3f; %.3f %.3f %.3f] t=[%.3f %.3f %.3f]",
                T_color_depth_.R[0], T_color_depth_.R[1], T_color_depth_.R[2],
                T_color_depth_.R[3], T_color_depth_.R[4], T_color_depth_.R[5],
                T_color_depth_.R[6], T_color_depth_.R[7], T_color_depth_.R[8],
                T_color_depth_.t[0], T_color_depth_.t[1], T_color_depth_.t[2]);
            has_extrinsics_ = true;
        }

        if (!has_depth_info_ || !has_color_info_)
        {
            RCLCPP_WARN_THROTTLE(this->get_logger(), *this->get_clock(), 2000,
                "Intrinsics no válidos aún, esperando");
            return;
        }
        
        
        std::vector<uint16_t> depth_input_mm;
        if (msg->encoding == "16UC1")
        {
            if (msg->data.size() != msg->width * msg->height * sizeof(uint16_t)) {
                RCLCPP_ERROR(this->get_logger(), "Depth data size mismatch (depth register node)");
                return;
            }

            depth_input_mm.resize(msg->width * msg->height);
            std::memcpy(depth_input_mm.data(), msg->data.data(), msg->data.size());
        }
        else if (msg->encoding == "32FC1")
        {
            if (msg->data.size() != msg->width * msg->height * sizeof(float)) {
                RCLCPP_ERROR(this->get_logger(), "Depth data size mismatch (depth register node)");
                return;
            }

            const auto *depth_m = reinterpret_cast<const float *>(msg->data.data());
            depth_input_mm.resize(msg->width * msg->height);
            for (size_t i = 0; i < depth_input_mm.size(); ++i)
            {
                const float value_m = depth_m[i];
                if (!std::isfinite(value_m) || value_m <= 0.0f)
                {
                    depth_input_mm[i] = 0;
                    continue;
                }

                const long value_mm = std::lround(value_m * 1000.0f);
                depth_input_mm[i] = static_cast<uint16_t>(
                    std::clamp(value_mm, 0L, static_cast<long>(std::numeric_limits<uint16_t>::max())));
            }
        }
        else
        {
            RCLCPP_ERROR(this->get_logger(),
                "Unsupported encoding: %s. Expected 16UC1 or 32FC1.",
                msg->encoding.c_str());
            return;
        }

        /*
        RCLCPP_INFO_THROTTLE(this->get_logger(), *this->get_clock(), 2000,
            "Processing depth frame: width=%d height=%d encoding=%s data_size=%zu",
            msg->width, msg->height, msg->encoding.c_str(), msg->data.size());
        */

        updateConstants(depth_K_, color_K_, T_color_depth_);

        depth_w_ = msg->width;
        depth_h_ = msg->height;

        if (color_w_ == 0 || color_h_ == 0 || depth_w_ == 0 || depth_h_ == 0)
        {
            RCLCPP_ERROR(this->get_logger(),
                "Invalid image dimensions: color_w=%d color_h=%d depth_w=%d depth_h=%d",
                color_w_, color_h_, depth_w_, depth_h_);
            return;
        }

        int depth_pixels = depth_w_ * depth_h_;
        int color_pixels = color_w_ * color_h_;
        ensureBuffers(depth_pixels, color_pixels);

        cudaMemcpy(
            d_depth_in_,
            depth_input_mm.data(),
            depth_pixels * sizeof(uint16_t),
            cudaMemcpyHostToDevice
        );

        registerDepthCUDA(
            d_depth_in_,
            d_z_buffer_,
            d_depth_out_,
            d_valid_in_,
            d_valid_out_,
            depth_w_,
            depth_h_,
            color_w_,
            color_h_
        );

        cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess) {
            RCLCPP_ERROR(this->get_logger(), "CUDA error: %s", cudaGetErrorString(err));
        }

        //cudaDeviceSynchronize();

        cudaMemcpy(
            depth_out_.data(),
            d_depth_out_,
            color_pixels * sizeof(uint16_t),
            cudaMemcpyDeviceToHost
        );

        sensor_msgs::msg::Image out_msg;
        out_msg.header = msg->header;
        out_msg.header.frame_id = target_frame_;

        out_msg.data.resize(color_w_ * color_h_ * sizeof(uint16_t));
        memcpy(out_msg.data.data(), depth_out_.data(), color_w_ * color_h_ * sizeof(uint16_t));
        out_msg.height = color_h_;
        out_msg.width = color_w_;
        out_msg.step = color_w_ * sizeof(uint16_t);
        out_msg.encoding = "16UC1";

        depth_pub_->publish(out_msg);
    }

    // ------------------------
    // ROS2
    // ------------------------

    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr depth_sub_;
    rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr color_info_sub_;
    rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr depth_info_sub_;

    std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
    std::shared_ptr<tf2_ros::TransformListener> tf_listener_;

    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr depth_pub_;
};

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<CudaDepthRegisterNode>());
    rclcpp::shutdown();
    return 0;
}