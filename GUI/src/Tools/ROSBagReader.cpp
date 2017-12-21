/*
 * This file is part of ElasticFusion.
 *
 * Copyright (C) 2015 Imperial College London
 *
 * The use of the code within this file and all code within files that
 * make up the software that is ElasticFusion is permitted for
 * non-commercial purposes only.  The full terms and conditions that
 * apply to the code within this file are detailed within the LICENSE.txt
 * file and at <http://www.imperial.ac.uk/dyson-robotics-lab/downloads/elastic-fusion/elastic-fusion-license/>
 * unless explicitly stated.  By downloading this file you agree to
 * comply with these terms.
 *
 * If you wish to use any of this code for commercial purposes then
 * please email researchcontracts.engineering@imperial.ac.uk.
 *
 */

#include "ROSBagReader.h"

bool isRosBag(std::string const& value)
{
    std::string ending = ".bag";
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

void rosGetParams(std::string const& filename, int& pixels_width, int& pixels_height, double& fx, double& fy, double& cx, double& cy) {
    rosbag::Bag bag;
    bag.open(filename, rosbag::bagmode::Read);
    
    // Pete ToDo: provice CLI for setting topic names
    std::string cam_info_topic = "/camera_1112170110/rgb/camera_info";
    std::vector<std::string> topics;
    topics.push_back(cam_info_topic);
    rosbag::View view(bag, rosbag::TopicQuery(topics));

    BOOST_FOREACH(rosbag::MessageInstance const m, view)
    { 
        if (m.getTopic() == cam_info_topic || ("/" + m.getTopic() == cam_info_topic)) {
            sensor_msgs::CameraInfo::ConstPtr cam_info = m.instantiate<sensor_msgs::CameraInfo>();
            if (cam_info != NULL) {
                pixels_width = cam_info->width;
                pixels_height = cam_info->height;
                fx = cam_info->K[0];
                fy = cam_info->K[4];
                cx = cam_info->K[2];
                cy = cam_info->K[5];
                bag.close();
                return;
            }
        }
    }
    std::cout << "Did not find camera info!" << std::endl;
    exit(0);
}

void loadBag(const std::string &filename, ROSRgbdData& log_rgbd_data)
{
  rosbag::Bag bag;
  bag.open(filename, rosbag::bagmode::Read);

  // Pete ToDo: provice CLI for setting topic names
  std::string image_d_topic = "/camera_1112170110/depth_registered/sw_registered/image_rect";
  std::string image_rgb_topic = "/camera_1112170110/rgb/image_rect_color";
  std::string cam_info_topic = "/camera_1112170110/rgb/camera_info";
  
  std::vector<std::string> topics;
  topics.push_back(image_d_topic);
  topics.push_back(image_rgb_topic);
  topics.push_back(cam_info_topic);
  
  rosbag::View view(bag, rosbag::TopicQuery(topics));
  
  BOOST_FOREACH(rosbag::MessageInstance const m, view)
  {
    if (m.getTopic() == image_d_topic || ("/" + m.getTopic() == image_d_topic))
    {
      sensor_msgs::Image::ConstPtr l_img = m.instantiate<sensor_msgs::Image>();
      if (l_img != NULL) {
        log_rgbd_data.images_d.push_back(l_img);
      }
    }
    
    if (m.getTopic() == image_rgb_topic || ("/" + m.getTopic() == image_rgb_topic))
    {
      sensor_msgs::Image::ConstPtr r_img = m.instantiate<sensor_msgs::Image>();
      if (r_img != NULL) {
        log_rgbd_data.images_rgb.push_back(r_img);
      }
    }
    
    if (m.getTopic() == cam_info_topic || ("/" + m.getTopic() == cam_info_topic))
    {
      sensor_msgs::CameraInfo::ConstPtr r_info = m.instantiate<sensor_msgs::CameraInfo>();
      if (r_info != NULL) {
        log_rgbd_data.cam_info = r_info;
      }
    }
  }
  bag.close();
  std::cout << "rgb data size " << log_rgbd_data.images_rgb.size() << std::endl;
  std::cout << "d data size " << log_rgbd_data.images_d.size() << std::endl;
}


ROSBagReader::ROSBagReader(std::string file, bool flipColors)
 : LogReader(file, flipColors)
{
    assert(pangolin::FileExists(file.c_str()));
    
    // Load in all of the ros bag into an ROSRgbdData strcut
    loadBag(file, log_rgbd_data);

    fp = fopen(file.c_str(), "rb");

    currentFrame = 0;

    // Pete ToDo: need to implement time sync
    // if (log_rgbd_data.images_rgb.size() != log_rgbd_data.images_d.size()) {
    //   std::cout << "Need to implement time sync!" << std::endl;
    //   exit(0);
    // }

    numFrames = log_rgbd_data.images_rgb.size();

    depthReadBuffer = new unsigned char[numPixels * 2];
    imageReadBuffer = new unsigned char[numPixels * 3];
    decompressionBufferDepth = new Bytef[numPixels * 2];
    decompressionBufferImage = new Bytef[numPixels * 3];
}

ROSBagReader::~ROSBagReader()
{
    delete [] depthReadBuffer;
    delete [] imageReadBuffer;
    delete [] decompressionBufferDepth;
    delete [] decompressionBufferImage;

    fclose(fp);
}

void ROSBagReader::getBack()
{
    currentFrame = numFrames;
    getCore();
}

void ROSBagReader::getNext()
{
    getCore();
}

void ROSBagReader::getCore()
{
    timestamp = log_rgbd_data.images_rgb.at(currentFrame)->header.stamp.toSec();
    depthSize = log_rgbd_data.images_d.at(currentFrame)->step * log_rgbd_data.images_d.at(currentFrame)->height;
    imageSize = log_rgbd_data.images_rgb.at(currentFrame)->step * log_rgbd_data.images_rgb.at(currentFrame)->height;

    // Depth 
    if ((depthSize == numPixels * 4) && (log_rgbd_data.images_d.at(currentFrame)->encoding == "32FC1")) 
    {
        // Encoding expected to be in ROS's standard CV_32FC1 format, 4 bytes per pixel as float
        // This convert to CV_16UC1 format, as expected by ElasticFusion
        cv::Mat cv_depth = cv::Mat(log_rgbd_data.images_d.at(0)->height, log_rgbd_data.images_d.at(0)->width, CV_32FC1); 
        for (size_t i = 0; i < log_rgbd_data.images_d.at(currentFrame)->height; i++) {
            for (size_t j = 0; j < log_rgbd_data.images_d.at(currentFrame)->width; j++) {
                cv_depth.at<float>(i,j) = *( (float*) &(log_rgbd_data.images_d.at(currentFrame)->data[0]) + (i*log_rgbd_data.images_d.at(currentFrame)->width + j) );
            }
        }
        cv::Mat cv_depth_out = cv::Mat(log_rgbd_data.images_d.at(0)->height, log_rgbd_data.images_d.at(0)->width, CV_16UC1);
        cv_depth.convertTo(cv_depth_out, CV_16UC1, 1000);
        memcpy(&decompressionBufferDepth[0], cv_depth_out.ptr(), numPixels * 2);
    }
    else
    {
        std::cout << "Am expecting 32FC1 encoded depth image in ROSBagReader.cpp" << std::endl;
        exit(0);
    }


    // RGB
    if (imageSize == numPixels * 3)
    {
        memcpy(&decompressionBufferImage[0], &(log_rgbd_data.images_rgb.at(currentFrame)->data[0]), numPixels * 3);
    } else {
        std::cout << "Am not expecting compressed images in ROSBagReader.cpp" << std::endl;
        exit(0);
    }

    rgb = (unsigned char *)&decompressionBufferImage[0];
    depth = (unsigned short *)&decompressionBufferDepth[0];

    if(flipColors)
    {
        for(int i = 0; i < Resolution::getInstance().numPixels() * 3; i += 3)
        {
            std::swap(rgb[i + 0], rgb[i + 2]);
        }
    }

    currentFrame++;
}

void ROSBagReader::fastForward(int frame)
{
  std::cout << "ROSBagReader::fastForward not implemented" << std::endl;
}

int ROSBagReader::getNumFrames()
{
    return numFrames;
}

bool ROSBagReader::hasMore()
{
    return currentFrame + 1 < numFrames;
}


void ROSBagReader::rewind()
{
    currentFrame = 0;
}

bool ROSBagReader::rewound()
{
    return (currentFrame == 0);
}

const std::string ROSBagReader::getFile()
{
    return file;
}

void ROSBagReader::setAuto(bool value)
{

}