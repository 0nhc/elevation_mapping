/*
 * ElevationMap.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: Péter Fankhauser
 *	 Institute: ETH Zurich, Autonomous Systems Lab
 */

#include "elevation_mapping/ElevationMap.hpp"

// Elevation Mapping
#include "elevation_mapping/ElevationMapFunctors.hpp"

// Grid Map
#include <grid_map_lib/GridMapMath.hpp>
#include <grid_map/GridMapMsgHelpers.hpp>
#include <grid_map_lib/SubmapIterator.hpp>

// Math
#include <math.h>

// ROS Logging
#include <ros/ros.h>

using namespace std;
using namespace Eigen;
using namespace sm;
using namespace sm::timing;

namespace elevation_mapping {

ElevationMap::ElevationMap(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle),
      rawMap_(vector<string>({"elevation", "variance", "horizontal_variance_x", "horizontal_variance_y", "color"})),
      fusedMap_(vector<string>({"elevation", "variance", "color"}))
{
  minVariance_ = 0.0;
  maxVariance_ = 0.0;
  mahalanobisDistanceThreshold_ = 0.0;
  multiHeightNoise_ = 0.0;
  minHorizontalVariance_ = 0.0;
  maxHorizontalVariance_ = 0.0;
  rawMap_.setClearTypes(vector<string>({"elevation", "variance"}));
  fusedMap_.setClearTypes(vector<string>({"elevation", "variance"}));
  reset();

  elevationMapRawPublisher_ = nodeHandle_.advertise<grid_map_msg::GridMap>("elevation_map_raw", 1);
  elevationMapFusedPublisher_ = nodeHandle_.advertise<grid_map_msg::GridMap>("elevation_map", 1);
}

ElevationMap::~ElevationMap()
{

}

void ElevationMap::setGeometry(const Eigen::Array2d& length, const double& resolution, const Eigen::Vector2d& position)
{
  boost::recursive_mutex::scoped_lock scopedLockForRawData(rawMapMutex_);
  boost::recursive_mutex::scoped_lock scopedLockForFusedData(fusedMapMutex_);
  rawMap_.setGeometry(length, resolution, position);
  fusedMap_.setGeometry(length, resolution, position);
  ROS_INFO_STREAM("Elevation map grid resized to " << rawMap_.getBufferSize()(0) << " rows and "  << rawMap_.getBufferSize()(1) << " columns.");
}

bool ElevationMap::add(const pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointCloud, Eigen::VectorXf& pointCloudVariances)
{
  for (unsigned int i = 0; i < pointCloud->size(); ++i)
  {
    auto& point = pointCloud->points[i];

    Array2i index;
    Vector2d position(point.x, point.y);
    if (!rawMap_.getIndex(position, index)) continue; // Skip this point if it does not lie within the elevation map.

    auto& elevation = rawMap_.at("elevation", index);
    auto& variance = rawMap_.at("variance", index);
    auto& horizontalVarianceX = rawMap_.at("horizontal_variance_x", index);
    auto& horizontalVarianceY = rawMap_.at("horizontal_variance_y", index);
    auto& color = rawMap_.at("color", index);
    float pointVariance = pointCloudVariances(i);

    if (!rawMap_.isValid(index))
    {
      // No prior information in elevation map, use measurement.
      elevation = point.z;
      variance = pointVariance;
      horizontalVarianceX = minHorizontalVariance_;
      horizontalVarianceY = minHorizontalVariance_;
      grid_map::colorVectorToValue(point.getRGBVector3i(), color);
      continue;
    }

    double mahalanobisDistance = sqrt(pow(point.z - elevation, 2) / variance);

    if (mahalanobisDistance < mahalanobisDistanceThreshold_)
    {
      // Fuse measurement with elevation map data.
      elevation = (variance * point.z + pointVariance * elevation) / (variance + pointVariance);
      variance =  (pointVariance * variance) / (pointVariance + variance);
      // TODO Add color fusion.
      grid_map::colorVectorToValue(point.getRGBVector3i(), color);
      continue;
    }

    // Add noise to cells which have ignored lower values,
    // such that outliers and moving objects are removed.
    variance += multiHeightNoise_;

    // Horizontal variances are reset.
    horizontalVarianceX = minHorizontalVariance_;
    horizontalVarianceY = minHorizontalVariance_;
  }

  clean();
  rawMap_.setTimestamp(1000 * pointCloud->header.stamp); // Point cloud stores time in microseconds.
  return true;
}

bool ElevationMap::update(Eigen::MatrixXf varianceUpdate, Eigen::MatrixXf horizontalVarianceUpdateX,
                          Eigen::MatrixXf horizontalVarianceUpdateY, const ros::Time& time)
{
  boost::recursive_mutex::scoped_lock scopedLock(rawMapMutex_);

  const auto& bufferSize = rawMap_.getBufferSize();

  if (!(
      (Array2i(varianceUpdate.rows(), varianceUpdate.cols()) == bufferSize).all() &&
      (Array2i(horizontalVarianceUpdateX.rows(), horizontalVarianceUpdateX.cols()) == bufferSize).all() &&
      (Array2i(horizontalVarianceUpdateY.rows(), horizontalVarianceUpdateY.cols()) == bufferSize).all()
      ))
  {
    ROS_ERROR("The size of the update matrices does not match.");
    return false;
  }

  rawMap_.get("variance") += varianceUpdate;
  rawMap_.get("horizontal_variance_x") += horizontalVarianceUpdateX;
  rawMap_.get("horizontal_variance_y") += horizontalVarianceUpdateY;
  clean();
  rawMap_.setTimestamp(time.toNSec());

  return true;
}

bool ElevationMap::fuseAll()
{
  ROS_DEBUG("Requested to fuse entire elevation map.");
  boost::recursive_mutex::scoped_lock scopedLock(fusedMapMutex_);
  return fuse(Array2i(0, 0), fusedMap_.getBufferSize());
}

bool ElevationMap::fuseArea(const Eigen::Vector2d& position, const Eigen::Array2d& length)
{
  ROS_DEBUG("Requested to fuse an area of the elevation map with center at (%f, %f) and side lenghts (%f, %f)",
            position[0], position[1], length[0], length[1]);

  Array2i topLeftIndex;
  Array2i submapBufferSize;

  // These parameters are not used in this function.
  Vector2d submapPosition;
  Array2d submapLength;
  Array2i requestedIndexInSubmap;

  boost::recursive_mutex::scoped_lock scopedLock(fusedMapMutex_);

  grid_map_lib::getSubmapInformation(topLeftIndex, submapBufferSize, submapPosition, submapLength, requestedIndexInSubmap, position, length,
                                     rawMap_.getLength(), rawMap_.getPosition(), rawMap_.getResolution(), rawMap_.getBufferSize(),
                                     rawMap_.getBufferStartIndex());

  return fuse(topLeftIndex, submapBufferSize);
}

bool ElevationMap::fuse(const Eigen::Array2i& topLeftIndex, const Eigen::Array2i& size)
{
  ROS_DEBUG("Fusing elevation map...");

  // Nothing to do.
  if (size.any() == 0) return false;

  // Initializations.
  string timerId = "map_fusion_timer";
  Timer timer(timerId, true);

  boost::recursive_mutex::scoped_lock scopedLock(fusedMapMutex_);

  // Copy raw elevation map data for safe multi-threading.
  boost::recursive_mutex::scoped_lock scopedLockForRawData(rawMapMutex_);
  auto rawMapCopy = rawMap_;
  scopedLockForRawData.unlock();

  // Check if there is the need to reset out-dated data.
  if (fusedMap_.getTimestamp() != rawMapCopy.getTimestamp()) resetFusedData();

  // For each cell in requested area.
  for (grid_map_lib::SubmapIterator areaIterator(rawMapCopy, topLeftIndex, size); !areaIterator.isPassedEnd(); ++areaIterator) {
    if (timer.isTiming()) timer.stop();
    timer.start();

    // Check if fusion for this cell has already been done earlier.
    if (fusedMap_.isValid(*areaIterator)) continue;

    if (!rawMapCopy.isValid(*areaIterator)) {
      // This is an empty cell (hole in the map).
      continue;
    }

    // Size of submap (2 sigma bound). TODO Add minimum/maximum submap size?
    Array2d submapLength = 4.0 * Array2d(rawMapCopy.at("horizontal_variance_x", *areaIterator), rawMapCopy.at("horizontal_variance_y", *areaIterator)).sqrt();

    // Requested position (center) of submap in map.
    Vector2d submapPosition;
    rawMapCopy.getPosition(*areaIterator, submapPosition);

    Array2i submapTopLeftIndex, submapBufferSize, requestedIndexInSubmap;

    grid_map_lib::getSubmapInformation(submapTopLeftIndex, submapBufferSize, submapPosition, submapLength, requestedIndexInSubmap, submapPosition, submapLength,
                         rawMapCopy.getLength(), rawMapCopy.getPosition(), rawMapCopy.getResolution(), rawMapCopy.getBufferSize(), rawMapCopy.getBufferStartIndex());

    // Prepare data fusion.
    ArrayXf means, variances, weights;
    int maxNumberOfCellsToFuse = submapBufferSize.prod();
    means.resize(maxNumberOfCellsToFuse);
    variances.resize(maxNumberOfCellsToFuse);
    weights.resize(maxNumberOfCellsToFuse);

    // For each cell in submap.
    size_t i = 0;
    for (grid_map_lib::SubmapIterator submapIterator(rawMapCopy, submapTopLeftIndex, submapBufferSize); !submapIterator.isPassedEnd(); ++submapIterator) {
      if (!rawMapCopy.isValid(*submapIterator)) {
        // Empty cell in submap (cannot be center cell because we checked above).
        continue;
      }

      means[i] = rawMapCopy.at("elevation", *submapIterator);
      variances[i] = rawMapCopy.at("variance", *submapIterator);

      // Compute weight from probability.
      Vector2d position;
      rawMapCopy.getPosition(*submapIterator, position);

      Vector2d distanceToCenter = (position - submapPosition).cwiseAbs();

      float probabilityX =
            cumulativeDistributionFunction(distanceToCenter.x() + rawMapCopy.getResolution() / 2.0, 0.0, sqrt(rawMapCopy.at("horizontal_variance_x", *submapIterator)))
          - cumulativeDistributionFunction(distanceToCenter.x() - rawMapCopy.getResolution() / 2.0, 0.0, sqrt(rawMapCopy.at("horizontal_variance_x", *submapIterator)));
      float probabilityY =
            cumulativeDistributionFunction(distanceToCenter.y() + rawMapCopy.getResolution() / 2.0, 0.0, sqrt(rawMapCopy.at("horizontal_variance_y", *submapIterator)))
          - cumulativeDistributionFunction(distanceToCenter.y() - rawMapCopy.getResolution() / 2.0, 0.0, sqrt(rawMapCopy.at("horizontal_variance_y", *submapIterator)));

      weights[i] = probabilityX * probabilityY;
      i++;
    }

    if (i == 0) {
      // Nothing to fuse.
      fusedMap_.at("elevation", *areaIterator) = rawMapCopy.at("elevation", *areaIterator);
      fusedMap_.at("variance", *areaIterator) = rawMapCopy.at("variance", *areaIterator);
      fusedMap_.at("color", *areaIterator) = rawMapCopy.at("color", *areaIterator);
      continue;
    }

    // Fuse.
    means.conservativeResize(i);
    variances.conservativeResize(i);
    weights.conservativeResize(i);

    float mean = (weights * means).sum() / weights.sum();
    float variance = (weights * (variances.square() + means.square())).sum() / weights.sum() - pow(mean, 2);

    if (!(std::isfinite(variance) && std::isfinite(mean)))
    {
      ROS_ERROR("Something went wrong when fusing the map: Mean = %f, Variance = %f", mean, variance);
      continue;
    }

    // Add to fused map.
    fusedMap_.at("elevation", *areaIterator) = mean;
    fusedMap_.at("variance", *areaIterator) = variance;

    // TODO Add fusion of colors.
    fusedMap_.at("color", *areaIterator) = rawMapCopy.at("color", *areaIterator);

    timer.stop();
  }

  fusedMap_.setTimestamp(rawMapCopy.getTimestamp());

  ROS_INFO("Elevation map has been fused in %f s.", Timing::getTotalSeconds(timerId));
  ROS_DEBUG("Mean: %f s, Min: %f s, Max: %f s.", Timing::getMeanSeconds(timerId), Timing::getMinSeconds(timerId), Timing::getMaxSeconds(timerId));
  Timing::reset(timerId);

  return true;
}

bool ElevationMap::reset()
{
  boost::recursive_mutex::scoped_lock scopedLockForRawData(rawMapMutex_);
  boost::recursive_mutex::scoped_lock scopedLockForFusedData(fusedMapMutex_);
  rawMap_.clearAll();
  fusedMap_.clearAll();
  return true;
}

void ElevationMap::move(const Eigen::Vector2d& position)
{
  boost::recursive_mutex::scoped_lock scopedLockForRawData(rawMapMutex_);
  rawMap_.move(position);
  scopedLockForRawData.unlock();

  // Do not move fused map if fusion is in process.
  boost::recursive_mutex::scoped_try_lock scopedLockForFusedData(fusedMapMutex_);
  if (scopedLockForFusedData.owns_lock()) fusedMap_.move(position);
}

bool ElevationMap::publishRawElevationMap()
{
  if (elevationMapRawPublisher_.getNumSubscribers() < 1) return false;
  grid_map_msg::GridMap message;
  rawMap_.toMessage(message);
  elevationMapRawPublisher_.publish(message);
  ROS_DEBUG("Elevation map raw has been published.");
  return true;
}

bool ElevationMap::publishElevationMap()
{
  if (elevationMapFusedPublisher_.getNumSubscribers() < 1) return false;
  boost::recursive_mutex::scoped_lock scopedLock(fusedMapMutex_);
  grid_map_msg::GridMap message;
  fusedMap_.toMessage(message);
  elevationMapFusedPublisher_.publish(message);
  ROS_DEBUG("Elevation map (fused) has been published.");
  return true;
}

grid_map::GridMap& ElevationMap::getRawGridMap()
{
  return rawMap_;
}

grid_map::GridMap& ElevationMap::getFusedGridMap()
{
  return fusedMap_;
}

ros::Time ElevationMap::getTimeOfLastUpdate()
{
  return ros::Time().fromNSec(rawMap_.getTimestamp());
}

ros::Time ElevationMap::getTimeOfLastFusion()
{
  boost::recursive_mutex::scoped_lock scopedLock(fusedMapMutex_);
  return ros::Time().fromNSec(fusedMap_.getTimestamp());
}

const kindr::poses::eigen_impl::HomogeneousTransformationPosition3RotationQuaternionD& ElevationMap::getPose()
{
  return pose_;
}

bool ElevationMap::getPosition3dInRobotParentFrame(const Eigen::Array2i& index, kindr::phys_quant::eigen_impl::Position3D& position)
{
  kindr::phys_quant::eigen_impl::Position3D positionInGridFrame;
  if (!rawMap_.getPosition3d("elevation", index, positionInGridFrame.vector())) return false;
  position = pose_.transform(positionInGridFrame);
  return true;
}

boost::recursive_mutex& ElevationMap::getFusedDataMutex()
{
  return fusedMapMutex_;
}

boost::recursive_mutex& ElevationMap::getRawDataMutex()
{
  return rawMapMutex_;
}

bool ElevationMap::clean()
{
  boost::recursive_mutex::scoped_lock scopedLockForRawData(rawMapMutex_);
  rawMap_.get("variance") = rawMap_.get("variance").unaryExpr(VarianceClampOperator<double>(minVariance_, maxVariance_));
  rawMap_.get("horizontal_variance_x") = rawMap_.get("horizontal_variance_x").unaryExpr(VarianceClampOperator<double>(minHorizontalVariance_, maxHorizontalVariance_));
  rawMap_.get("horizontal_variance_y") = rawMap_.get("horizontal_variance_y").unaryExpr(VarianceClampOperator<double>(minHorizontalVariance_, maxHorizontalVariance_));
  return true;
}

void ElevationMap::resetFusedData()
{
  boost::recursive_mutex::scoped_lock scopedLockForFusedData(fusedMapMutex_);
  fusedMap_.clearAll();
  fusedMap_.setTimestamp(0);
}

void ElevationMap::setFrameId(const std::string& frameId)
{
  rawMap_.setFrameId(frameId);
  fusedMap_.setFrameId(frameId);
}

const std::string& ElevationMap::getFrameId()
{
  return rawMap_.getFrameId();
}

float ElevationMap::cumulativeDistributionFunction(float x, float mean, float standardDeviation)
{
  return 0.5 * erfc(-(x-mean)/(standardDeviation*sqrt(2.0)));
}

} /* namespace */
