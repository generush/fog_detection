#include <fog_detection/fog_detection.h>

namespace fog
{
    void FogDetectionNodelet::connectCb()
    {
    };

    void FogDetectionNodelet::onInit()
    {
        nh         = getMTNodeHandle();
        private_nh = getMTPrivateNodeHandle();

        it_in_ .reset(new image_transport::ImageTransport(nh));

        // Read parameters
        private_nh.param("queue_size", queue_size_, 5);
        private_nh.param("target_frame_id", target_frame_id_, std::string());

        // Set up dynamic reconfigure
        reconfigure_server_.reset(new ReconfigureServer(config_mutex_, private_nh));
        ReconfigureServer::CallbackType f = boost::bind(&FogDetectionNodelet::configCb, this, _1, _2);
        reconfigure_server_->setCallback(f);

	    // Monitor whether anyone is subscribed to the h_scans
        image_transport::SubscriberStatusCallback connect_cb = boost::bind(&FogDetectionNodelet::connectCb, this);
        ros::SubscriberStatusCallback connect_cb_info = boost::bind(&FogDetectionNodelet::connectCb, this);

        // Make sure we don't enter connectCb() between advertising and assigning to pub_h_scans_
        boost::lock_guard<boost::mutex> lock(connect_mutex_);
        image_transport::TransportHints hints("raw", ros::TransportHints(), getPrivateNodeHandle());

        // Subscriber Ouster images
        sub_range_img_ = nh.subscribe("in_range_img", 1, &FogDetectionNodelet::range_image_cb, this);
        sub_intensity_img_ = nh.subscribe("in_intensity_img", 1, &FogDetectionNodelet::intensity_image_cb, this);

        // sub_low_depth_pcl_ = nh.subscribe("in_pcl", 1,  &FogDetectionNodelet::point_cloud_cb, this);
        sub_low_depth_pcl_ = nh.subscribe<sensor_msgs::PointCloud2>("in_pcl", 500, boost::bind(&FogDetectionNodelet::point_cloud_cb, this, _1));

        // Publish Point Cloud
        pub_avg_range_img_          = private_nh.advertise<sensor_msgs::Image>("out_avg_range_img", 10);
        pub_diff_range_img_         = private_nh.advertise<sensor_msgs::Image>("out_diff_range_img", 10);
        pub_var_range_img_          = private_nh.advertise<sensor_msgs::Image>("out_var_range_img", 10);
        pub_sum_noreturn_img_       = private_nh.advertise<sensor_msgs::Image>("out_sum_noreturn_img", 10);
        pub_dev_range_img_          = private_nh.advertise<sensor_msgs::Image>("out_dev_range_img", 10);
        pub_dev_diff_range_img_     = private_nh.advertise<sensor_msgs::Image>("out_dev_diff_range_img", 10);
        pub_var_t_img_              = private_nh.advertise<sensor_msgs::Image>("out_var_t_img", 10);
        pub_noreturn_img_           = private_nh.advertise<sensor_msgs::Image>("out_noreturn_img", 10);
        pub_noreturn_lowres_img_    = private_nh.advertise<sensor_msgs::Image>("out_noreturn_lowres_img", 10);
        pub_prob_noreturn_img_      = private_nh.advertise<sensor_msgs::Image>("prob_noreturn_img", 10);
        pub_range_img_              = private_nh.advertise<sensor_msgs::Image>("out_range_img", 10);
        pub_intensity_img_          = private_nh.advertise<sensor_msgs::Image>("out_intensity_img", 10);
        pub_log_intensity_img_      = private_nh.advertise<sensor_msgs::Image>("out_log_intensity_img", 10);
        pub_intensity_filter_img_   = private_nh.advertise<sensor_msgs::Image>("out_intensity_filter_img", 10);
        pub_intensity_filter_pcl_   = private_nh.advertise<PointCloud>("out_intensity_filter_pcl", 10);
        pub_normal_pcl_             = private_nh.advertise<PointCloud>("out_normal_pcl", 10);
        pub_test_pcl_               = private_nh.advertise<PointCloud>("out_test_pcl", 10);

        // Create static tf broadcaster (-30 pitch, Realsense pointed down)
        // rosrun tf static_transform_publisher 0.0 0.0 0.0 0.0 -0.00913852259 0.0 base_link royale_camera_optical_frame 1000

        try
        {
            low_listener.waitForTransform(low_sensor_frame, low_robot_frame, ros::Time(0), ros::Duration(10.0) );
            low_listener.lookupTransform(low_sensor_frame, low_robot_frame, ros::Time(0), low_transform);
            low_transform.setOrigin(tf::Vector3(0.0, 0.0, 0.0));
	        low_m_euler.setRotation(low_transform.getRotation());
            int low_solution_number = 1;
            low_m_euler.getEulerYPR(low_yaw, low_pitch, low_roll, low_solution_number);
        }
        catch (tf::TransformException ex)
        {
            ROS_WARN("%s",ex.what());
        }

        std::cout << "base_link->camera roll: " << low_roll << std::endl;
        std::cout << "base_link->camera pitch: " << low_pitch << std::endl;
        std::cout << "base_link->camera yaw: " << low_yaw << std::endl;
    };

    void FogDetectionNodelet::configCb(Config &config, uint32_t level)
    {
        config_ = config;

        // Depth Camera Parameters
        low_robot_frame            = config.low_robot_frame;
        low_sensor_frame           = config.low_sensor_frame;

        // Depth Image Parameters
        low_camera_pixel_x_offset  = config.low_camera_pixel_x_offset;
        low_camera_pixel_y_offset  = config.low_camera_pixel_y_offset;
        low_camera_pixel_width     = config.low_camera_pixel_width;
        low_camera_pixel_height    = config.low_camera_pixel_height;

        transform_pcl_roll_         = config.transform_pcl_roll;
        transform_pcl_pitch_        = config.transform_pcl_pitch;
        transform_pcl_yaw_          = config.transform_pcl_yaw;
        normal_radius_              = config.normal_radius;
        normal_x_LT_threshold_      = config.normal_x_LT_threshold;
        normal_x_GT_threshold_      = config.normal_x_GT_threshold;
        normal_y_LT_threshold_      = config.normal_y_LT_threshold;
        normal_y_GT_threshold_      = config.normal_y_GT_threshold;
        normal_z_LT_threshold_      = config.normal_z_LT_threshold;
        normal_z_GT_threshold_      = config.normal_z_GT_threshold;
        ror_radius_                 = config.ror_radius;
        ror_min_neighbors_          = config.ror_min_neighbors;

    };

    void FogDetectionNodelet::range_image_cb(const sensor_msgs::ImageConstPtr& image_msg)
    {
        // analyze_range_images(image_msg,
        //                      pub_range_img_,
        //                      low_camera_pixel_x_offset,
        //                      low_camera_pixel_y_offset,
        //                      low_camera_pixel_width,
        //                      low_camera_pixel_height
        //                      );
    };

    void FogDetectionNodelet::intensity_image_cb(const sensor_msgs::ImageConstPtr& image_msg)
    {
        analyze_intensity_images(image_msg,
                                 pub_intensity_img_,
                                 low_camera_pixel_x_offset,
                                 low_camera_pixel_y_offset,
                                 low_camera_pixel_width,
                                 low_camera_pixel_height
                                 );
    };

    void FogDetectionNodelet::point_cloud_cb(const sensor_msgs::PointCloud2::ConstPtr& cloud_in_ros)
    {
        
        bool range_pcl = 0;

        // Get the field structure of this point cloud
        for (int f=0; f<cloud_in_ros->fields.size(); f++)
        {
            if (cloud_in_ros->fields[f].name == "range")
            {
                range_pcl = 1;
            }
        }

        if(range_pcl == 1)
        {

            seq++;

            ouster_ros::OS1::CloudOS1 cloud_in{};
            pcl::fromROSMsg(*cloud_in_ros, cloud_in);

            pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_in2 (new pcl::PointCloud<pcl::PointXYZI>);
            pcl::fromROSMsg(*cloud_in_ros, *cloud_in2);
            
            sensor_msgs::Image range_image;
            sensor_msgs::Image noise_image;
            sensor_msgs::Image intensity_image;

            // Get PCL metadata
            int W = cloud_in_ros->width;
            int H = cloud_in_ros->height;
            std::vector<int>  px_offset = ouster::OS1::get_px_offset(W);
            // for 64 channels, the px_offset =[ 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 
            //                                   0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 12, 18]

            // Setup Noise Image
            noise_image.width = W;
            noise_image.height = H;
            noise_image.step = W;
            noise_image.encoding = "mono8";
            noise_image.data.resize(W * H);

            // Set up Intensity Image
            intensity_image.width = W;
            intensity_image.height = H;
            intensity_image.step = W;
            intensity_image.encoding = "mono8";
            intensity_image.data.resize(W * H);

            cv::Mat img(H, W, CV_32FC(10), 0.0);
            cv::Mat img_x(H, W, CV_32FC1, 0.1);
            cv::Mat img_y(H, W, CV_32FC1, 0.1);
            cv::Mat img_z(H, W, CV_32FC1, 0.1);
            cv::Mat img_d(H, W, CV_32FC1, 0.1);
            cv::Mat img_i(H, W, CV_32FC1, 0.1);
            cv::Mat img_r(H, W, CV_32FC1, 0.1);
            cv::Mat img_n(H, W, CV_32FC1, 0.1);

            cv::Mat zero_range_img(H, W, CV_32FC1, 0.1);
            cv::Mat max_range_img(H, W, CV_32FC1, 20.0);
            cv::Mat nonzero_range_img(H, W, CV_32FC1, 0.1);
            cv::Mat range_img(H, W, CV_32FC1, 0.0);
            cv::Mat intensity_img(H, W, CV_32FC1, 0.0);
            cv::Mat log_intensity_img(H, W, CV_32FC1, 0.0);
            cv::Mat sq_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat sq_of_sum_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat zeroreturn_img(H, W, CV_32FC1, 0.0);
            cv::Mat acc_noreturn_msk(H, W, CV_32FC1, 0.0);
            cv::Mat avg_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat diff_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat var_s_raw_img(H, W, CV_32FC1, 0.0);
            cv::Mat var_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat dev_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat dev_range_bin_img(H, W, CV_32FC1, 0.0);
            cv::Mat dev_diff_range_img(H, W, CV_32FC1, 0.0);
            cv::Mat var_t_img(H, W, CV_32FC1, 0.0);
            cv::Mat return_img(H, W, CV_32FC1, 0.0);
            cv::Mat return_sum_img(H, W, CV_32FC1, 0.0);
            cv::Mat noreturn_img(H, W, CV_32FC1, 0.0);
            cv::Mat prob_noreturn_img(H, W, CV_32FC1, 0.0);

            cv::Mat zero_range_msk(H, W, CV_32FC1, 0.0);

            cv::Mat intensity_filter_img(H, W, CV_32FC1, 0.0);

            float dist = 0;
            float max_dist = 0;
            float last_dist = 0;

            for (int u = 0; u < H; u++) {
                for (int v = 0; v < W; v++) {
                    const size_t vv = (v + px_offset[u]) % W;
                    const size_t index = vv * H + u;
                    const auto& pt = cloud_in[index];
                    range_img.at<float>(u,v) = pt.range * 1e-3; // Physical range from 0 - 100 m (converting from mm --> m)
                    // range_img.at<float>(u,v) = pt.range * 5e-3; // Range for 8-bit image from 0 - 255
                    noise_image.data[u * W + v] = std::min(pt.noise, (uint16_t)255);
                    intensity_image.data[u * W + v] = std::min(pt.intensity, 255.f);
                    intensity_img.at<float>(u,v) = pt.intensity + 1.0f;

                    img_x.at<float>(u,v) = pt.x;
                    img_y.at<float>(u,v) = pt.y;
                    img_z.at<float>(u,v) = pt.z;
                    img_d.at<float>(u,v) = pt.range * 1e-3;
                    img_i.at<float>(u,v) = pt.intensity;
                    img_r.at<float>(u,v) = pt.reflectivity;
                    img_n.at<float>(u,v) = pt.noise;
                }
            }

            ////////////////////////////
            // INTENSITY FILTER (PCL) //
            ////////////////////////////

            cv::threshold(intensity_img, intensity_filter_img, 100.0, 1.0, THRESH_BINARY_INV);

            // Intensity Filter
            pcl::PointIndices::Ptr inliers(new pcl::PointIndices());
            pcl::ExtractIndices<pcl::PointXYZI> extract;

            for (int u = 0; u < H; u++)
            {
                for (int v = 0; v < W; v++)
                {
                    if(intensity_filter_img.at<float>(u,v) > 0.1)
                    {
                        const size_t vv = (v + px_offset[u]) % W;
                        const size_t i = vv * H + u;
                        inliers->indices.push_back(i);
                    }
                }
            }

            extract.setInputCloud(cloud_in2);
            extract.setIndices(inliers);
            extract.setNegative(false);
            pcl::PointCloud<pcl::PointXYZI>::Ptr output(new pcl::PointCloud<pcl::PointXYZI>);
            extract.filter(*output);

            output->width = output->points.size ();
            output->height = 1;
            output->is_dense = true;
            output->header.seq = seq;
            output->header.frame_id = cloud_in2->header.frame_id;
            pcl_conversions::toPCL(ros::Time::now(), output->header.stamp);
            pub_intensity_filter_pcl_.publish (output);
            
            /////////////////////////
            // CALCULATE PCA (IMG) //
            /////////////////////////

            // int window_sz = 3;
            // int ofst = (window_sz - 1) / 2;

            // for (int u = 0 + ofst; u < H - ofst; u++) {
            //     for (int v = 0 + ofst; v < W - ofst; v++) {
            //         const size_t vv = (v + px_offset[u]) % W;
            //         const size_t index = vv * H + u;
            //         const auto& pt = cloud_in[index];
            //     }
            // }

            // Test Filter
            pcl::PointIndices::Ptr test_pcl(new pcl::PointIndices());
            pcl::ExtractIndices<pcl::PointXYZI> test_extract;

            for (int u = 0; u < H; u++)
            {
                for (int v = 0; v < W; v++)
                {
                    if(img_x.at<float>(u,v) > 0.0)
                    {
                        const size_t vv = (v + px_offset[u]) % W;
                        const size_t i = vv * H + u;
                        test_pcl->indices.push_back(i);
                    }
                }
            }

            test_extract.setInputCloud(cloud_in2);
            test_extract.setIndices(test_pcl);
            test_extract.setNegative(false);
            pcl::PointCloud<pcl::PointXYZI>::Ptr test_output(new pcl::PointCloud<pcl::PointXYZI>);
            test_extract.filter(*test_output);
      
            test_output->width = test_output->points.size ();
            test_output->height = 1;
            test_output->is_dense = true;
            test_output->header.seq = seq;
            test_output->header.frame_id = cloud_in2->header.frame_id;
            pcl_conversions::toPCL(ros::Time::now(), test_output->header.stamp);
            pub_test_pcl_.publish (test_output);

            // Publish Range Image
            cv_bridge::CvImage new_range_msg;
            new_range_msg.encoding                  = sensor_msgs::image_encodings::TYPE_32FC1;
            new_range_msg.image                     = range_img;
            new_range_msg.header.stamp              = ros::Time::now();
            new_range_msg.header.frame_id           = cloud_in_ros->header.frame_id;        
            pub_range_img_.publish(new_range_msg.toImageMsg());

            // Publish Intensity Image
            cv_bridge::CvImage intensity_msg;
            intensity_msg.encoding              = sensor_msgs::image_encodings::TYPE_32FC1;
            intensity_msg.image                 = intensity_img;
            intensity_msg.header.stamp          = ros::Time::now();
            intensity_msg.header.frame_id       = cloud_in_ros->header.frame_id;        
            pub_intensity_img_.publish(intensity_msg.toImageMsg());

            // Publish Intensity Filter
            cv_bridge::CvImage intensity_filter_msg;
            intensity_filter_msg.encoding                   = sensor_msgs::image_encodings::TYPE_32FC1;
            intensity_filter_msg.image                      = intensity_filter_img;
            intensity_filter_msg.header.stamp               = ros::Time::now();
            intensity_filter_msg.header.frame_id            = cloud_in_ros->header.frame_id;        
            pub_intensity_filter_img_.publish(intensity_filter_msg.toImageMsg());

            ///////////////////////
            // CALCULATE NORMALS //
            ///////////////////////

            bool flag_pub_normal_pcl = 0;
            
            if(flag_pub_normal_pcl)
            {

                pcl::search::Search<pcl::PointXYZI>::Ptr tree_xyz;

                // Use KDTree for non-organized data
                tree_xyz.reset(new pcl::search::KdTree<pcl::PointXYZI> (false));

                // if (cloud_in2->isOrganized ())
                // {
                //     std::cout << "org" << std::endl;
                //     tree_xyz.reset(new pcl::search::OrganizedNeighbor<pcl::PointXYZI> ());
                // }
                // else
                // {
                //     std::cout << "not org" << std::endl;
                //     // Use KDTree for non-organized data
                //     tree_xyz.reset(new pcl::search::KdTree<pcl::PointXYZI> (false));
                // }

                // Set input pointcloud for search tree
                tree_xyz->setInputCloud(cloud_in2);

                pcl::NormalEstimationOMP<pcl::PointXYZI, pcl::PointXYZINormal> ne; // NormalEstimationOMP uses more CPU on NUC (do not use OMP!)
                ne.setInputCloud(cloud_in2);
                ne.setSearchMethod(tree_xyz);

                // Set viewpoint, very important so normals are all pointed in the same direction
                ne.setViewPoint(std::numeric_limits<float>::max (), std::numeric_limits<float>::max (), std::numeric_limits<float>::max ());

                pcl::PointCloud<pcl::PointXYZINormal>::Ptr cloud_ne (new pcl::PointCloud<pcl::PointXYZINormal>);

                ne.setRadiusSearch(0.05);
                ne.compute(*cloud_ne);

                // Assign original xyz data to normal estimate cloud (this is necessary because by default the xyz fields are empty)
                for (int i = 0; i < cloud_in2->points.size(); i++)
                {
                    cloud_ne->points[i].x = cloud_in2->points[i].x;
                    cloud_ne->points[i].y = cloud_in2->points[i].y;
                    cloud_ne->points[i].z = cloud_in2->points[i].z;
                    cloud_ne->points[i].intensity = cloud_in2->points[i].intensity;
                }
                
                cloud_ne->width = cloud_ne->points.size ();
                cloud_ne->height = 1;
                cloud_ne->is_dense = true;
                cloud_ne->header.seq = seq;
                cloud_ne->header.frame_id = cloud_in2->header.frame_id;
                pcl_conversions::toPCL(ros::Time::now(), cloud_ne->header.stamp);
                pub_normal_pcl_.publish (cloud_ne);

            }

            



            
        }
    
    };

    void FogDetectionNodelet::analyze_range_images(const sensor_msgs::ImageConstPtr& in_msg,
                                                   const ros::Publisher pub_img_,
                                                   int x_offset,
                                                   int y_offset,
                                                   int width,
                                                   int height)
    {

        // Get cropping parameters
        int max_width = in_msg->width - x_offset;
        int max_height = in_msg->height - y_offset;
        if (width == 0 || width > max_width)
            width = max_width;
        if (height == 0 || height > max_height)
            height = max_height;

        // Convert from ROS to OpenCV
        CvImageConstPtr source = toCvShare(in_msg);

        // Crop image
        cv::Mat range_img = source->image(cv::Rect(x_offset, y_offset, width, height));

        // The range image is of type CV_8UC1/mono8 (0-255, 0 is no return, higher is closer to sensor)
        range_img.convertTo(range_img,CV_32FC1, 0.39215686274); // 100/255 = 0.39215686274

        cv::Mat diff_range_img = abs(range_img - last_range_img);

        last_range_img = range_img;

    };

    void FogDetectionNodelet::analyze_intensity_images(const sensor_msgs::ImageConstPtr& in_msg,
                                                       const ros::Publisher pub_img_,
                                                       int x_offset,
                                                       int y_offset,
                                                       int width,
                                                       int height)
    {

        // Get cropping parameters
        int max_width = in_msg->width - x_offset;
        int max_height = in_msg->height - y_offset;
        if (width == 0 || width > max_width)
            width = max_width;
        if (height == 0 || height > max_height)
            height = max_height;

        // Convert from ROS to OpenCV
        CvImageConstPtr source = toCvShare(in_msg);

        // Crop image
        cv::Mat new_intensity_img = source->image(cv::Rect(x_offset, y_offset, width, height));

        // The intensity image is of type CV_8UC1/mono8 (0-255)
        new_intensity_img.convertTo(new_intensity_img, CV_32FC1, 0.39215686274);

        cv::Mat diff_intensity_img = abs(new_intensity_img - last_intensity_img);

        last_intensity_img = new_intensity_img;

    };
    
    namespace detail
    {
        template <typename Vector, typename Scalar>
        struct EigenVector 
        {
            Vector vector;
            Scalar length;
        };  // struct EigenVector
        
        /**
         * @brief returns the unit vector along the largest eigen value as well as the
         *        length of the largest eigenvector
         * @tparam Vector Requested result type, needs to be explicitly provided and has
         *                to be implicitly constructible from ConstRowExpr
         * @tparam Matrix deduced input type providing similar in API as Eigen::Matrix
         */
        template <typename Vector, typename Matrix> static EigenVector<Vector, typename Matrix::Scalar>
        getLargest3x3Eigenvector(const Matrix scaledMatrix)
        {
            using Scalar = typename Matrix::Scalar;
            using Index = typename Matrix::Index;
            
            Matrix crossProduct;
            crossProduct << scaledMatrix.row (0).cross (scaledMatrix.row (1)),
                            scaledMatrix.row (0).cross (scaledMatrix.row (2)),
                            scaledMatrix.row (1).cross (scaledMatrix.row (2));
            
            // expression template, no evaluation here
            const auto len = crossProduct.rowwise ().norm ();
            
            Index index;
            const Scalar length = len.maxCoeff (&index);  // <- first evaluation
            return EigenVector<Vector, Scalar> {crossProduct.row (index) / length,
                                                length};
        }
    }
        
    template <typename Matrix, typename Vector> inline void
    FogDetectionNodelet::eigen33(const Matrix& mat, typename Matrix::Scalar& eigenvalue, Vector& eigenvector)
    {
        using Scalar = typename Matrix::Scalar;
        // Scale the matrix so its entries are in [-1,1].  The scaling is applied
        // only when at least one matrix entry has magnitude larger than 1.
        
        Scalar scale = mat.cwiseAbs ().maxCoeff ();
        if (scale <= std::numeric_limits < Scalar > ::min ())
            scale = Scalar (1.0);
        
        Matrix scaledMat = mat / scale;
        
        Vector eigenvalues;
        computeRoots (scaledMat, eigenvalues);
        
        eigenvalue = eigenvalues (0) * scale;
        
        scaledMat.diagonal ().array () -= eigenvalues (0);
        
        eigenvector = detail::getLargest3x3Eigenvector<Vector> (scaledMat).vector;
    }
    
    template <typename Scalar> inline unsigned int
    FogDetectionNodelet::computeMeanAndCovarianceMatrix(cv::Mat &x,
                                                        cv::Mat &y,
                                                        cv::Mat &z,
                                                        Eigen::Matrix<Scalar, 3, 3> &covariance_matrix,
                                                        Eigen::Matrix<Scalar, 4, 1> &centroid)
    {
        // create the buffer on the stack which is much faster than using cloud[indices[i]] and centroid as a buffer
        Eigen::Matrix<Scalar, 1, 9, Eigen::RowMajor> accu = Eigen::Matrix<Scalar, 1, 9, Eigen::RowMajor>::Zero ();
        
        accu [0] = x.dot(x);
        accu [1] = x.dot(y);
        accu [2] = x.dot(z);
        accu [3] = y.dot(y);
        accu [4] = y.dot(z);
        accu [5] = z.dot(z);
        accu [6] = cv::sum(x)[0];
        accu [7] = cv::sum(y)[0];
        accu [8] = cv::sum(z)[0];

        //Eigen::Vector3f vec = accu.tail<3> ();
        //centroid.head<3> () = vec;//= accu.tail<3> ();
        //centroid.head<3> () = accu.tail<3> ();    -- does not compile with Clang 3.0
        centroid[0] = accu[6]; centroid[1] = accu[7]; centroid[2] = accu[8];
        centroid[3] = 1;
        covariance_matrix.coeffRef (0) = accu [0] - accu [6] * accu [6];
        covariance_matrix.coeffRef (1) = accu [1] - accu [6] * accu [7];
        covariance_matrix.coeffRef (2) = accu [2] - accu [6] * accu [8];
        covariance_matrix.coeffRef (4) = accu [3] - accu [7] * accu [7];
        covariance_matrix.coeffRef (5) = accu [4] - accu [7] * accu [8];
        covariance_matrix.coeffRef (8) = accu [5] - accu [8] * accu [8];
        covariance_matrix.coeffRef (3) = covariance_matrix.coeff (1);
        covariance_matrix.coeffRef (6) = covariance_matrix.coeff (2);
        covariance_matrix.coeffRef (7) = covariance_matrix.coeff (5);
        
        int point_count = x.total();

        return (static_cast<unsigned int> (point_count));
    }
    
    inline void
    FogDetectionNodelet::solvePlaneParameters(const Eigen::Matrix3f &covariance_matrix,
                        float &nx, float &ny, float &nz, float &curvature)
    {
    // Avoid getting hung on Eigen's optimizers
    //  for (int i = 0; i < 9; ++i)
    //    if (!std::isfinite (covariance_matrix.coeff (i)))
    //    {
    //      //PCL_WARN ("[pcl::solvePlaneParameteres] Covariance matrix has NaN/Inf values!\n");
    //      nx = ny = nz = curvature = std::numeric_limits<float>::quiet_NaN ();
    //      return;
    //    }
    // Extract the smallest eigenvalue and its eigenvector
    EIGEN_ALIGN16 Eigen::Vector3f::Scalar eigen_value;
    EIGEN_ALIGN16 Eigen::Vector3f eigen_vector;
    pcl::eigen33(covariance_matrix, eigen_value, eigen_vector);
    
    nx = eigen_vector [0];
    ny = eigen_vector [1];
    nz = eigen_vector [2];
    
    // Compute the curvature surface change
    float eig_sum = covariance_matrix.coeff (0) + covariance_matrix.coeff (4) + covariance_matrix.coeff (8);
    if (eig_sum != 0)
        curvature = std::abs (eigen_value / eig_sum);
    else
        curvature = 0;
    }
  

    /** \brief Compute the Least-Squares plane fit for a given set of points, using their indices,
     * and return the estimated plane parameters together with the surface curvature.
     * \param cloud the input point cloud
     * \param indices the point cloud indices that need to be used
     * \param nx the resultant X component of the plane normal
     * \param ny the resultant Y component of the plane normal
     * \param nz the resultant Z component of the plane normal
     * \param curvature the estimated surface curvature as a measure of
     * \f[
     * \lambda_0 / (\lambda_0 + \lambda_1 + \lambda_2)
     * \f]
     */

    inline bool
    FogDetectionNodelet::computePointNormal(cv::Mat &x,
                                            cv::Mat &y,
                                            cv::Mat &z,
                                            const std::vector<int> &indices,
                                            float &nx, float &ny, float &nz, float &curvature)
    {

        // Placeholder for the 3x3 covariance matrix at each surface patch
        EIGEN_ALIGN16 Eigen::Matrix3f covariance_matrix_;

       /** \brief 16-bytes aligned placeholder for the XYZ centroid of a surface patch. */
       Eigen::Vector4f xyz_centroid_;
               
        if (indices.size () < 3 || 
            computeMeanAndCovarianceMatrix(x, y, z, covariance_matrix_, xyz_centroid_) == 0)
        {
            nx = ny = nz = curvature = std::numeric_limits<float>::quiet_NaN ();
            return false;
        }

        // Get the plane normal and surface curvature
        FogDetectionNodelet::solvePlaneParameters(covariance_matrix_, nx, ny, nz, curvature);
        return true;
    }


}

// Register nodelet
#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS( fog::FogDetectionNodelet, nodelet::Nodelet)
