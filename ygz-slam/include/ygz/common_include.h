#ifndef COMMON_INCLUDE_H_
#define COMMON_INCLUDE_H_

// std 
#include <vector>
#include <list>
#include <memory>
#include <string>
#include <iostream>
#include <set>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <mutex>
#include <thread>

using namespace std; 

// define the commonly included file to avoid a long include list
// for Eigen
#include <Eigen/Core>
#include <Eigen/Geometry>
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::Matrix2f;
using Eigen::Matrix3f;
using Eigen::Matrix4f;
// other things I need in optimiztion 
typedef Eigen::Matrix<double, 6, 1> Vector6d; 

#include <Eigen/StdVector> // for vector of Eigen objects 
typedef vector<Vector3d, Eigen::aligned_allocator<Vector3d>> VecVector3d;

// for Sophus
#include <sophus/se3.h>
#include <sophus/so3.h>
using Sophus::SO3;
using Sophus::SE3;

// for cv
#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/highgui/highgui.hpp>
using cv::Mat;

// for glog
#include <glog/logging.h>


// ceres
#include <ceres/ceres.h>

#endif
