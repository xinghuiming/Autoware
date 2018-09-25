/*
 * MeidaiLocalizerNDT.cpp
 *
 *  Created on: Sep 16, 2018
 *      Author: sujiwo
 */


#include <datasets/MeidaiBagDataset.h>
#include <iostream>
#include <string>
#include <vector>
#include <exception>

#include <ros/ros.h>
#include <pcl/point_types.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <velodyne_pointcloud/rawdata.h>

#include "NdtLocalizer.h"


using namespace std;
using velodyne_rawdata::VPoint;
using velodyne_rawdata::VPointCloud;
using pcl::PointCloud;
using pcl::PointXYZ;


/*
 * XXX: These values may need to be adjusted
 */
const float
	velodyneMinRange = 2.0,
	velodyneMaxRange = 130,
	velodyneViewDirection = 0,
	velodyneViewWidth = 2*M_PI;

const NdtLocalizerInitialConfig NuInitialConfig = {
	0,0,0, 0,0,0
};

// Velodyne HDL-64 calibration file
const string paramFileTest = "/home/sujiwo/Autoware/ros/src/computing/perception/localization/packages/vmml/params/64e-S2.yaml";
const string meidaiMapPcd = "/home/sujiwo/Data/NagoyaUniversityMap/bin_meidai_ndmap.pcd";


class VelodynePreprocessor
{
public:

	VelodynePreprocessor(const string &lidarCalibFile):

		 data_(new velodyne_rawdata::RawData())
	{
		data_->setupOffline(lidarCalibFile, velodyneMaxRange, velodyneMinRange);
		data_->setParameters(velodyneMinRange, velodyneMaxRange, velodyneViewDirection, velodyneViewWidth);
	}

	PointCloud<PointXYZ>::ConstPtr
	convertMessage(velodyne_msgs::VelodyneScan::ConstPtr bagmsg)
	{
		VPointCloud::Ptr outPoints(new VPointCloud);
		outPoints->header.stamp = pcl_conversions::toPCL(bagmsg->header).stamp;
		outPoints->header.frame_id = bagmsg->header.frame_id;
		outPoints->height = 1;

		for (int i=0; i<bagmsg->packets.size(); ++i) {
			data_->unpack(bagmsg->packets[i], *outPoints, bagmsg->packets.size());
		}

		PointCloud<PointXYZ>::Ptr cloudTmp = convertToExternal(*outPoints);
		return VoxelGridFilter(cloudTmp, 0.2, 3.0);
	}

	PointCloud<PointXYZ>::ConstPtr
	VoxelGridFilter (PointCloud<PointXYZ>::ConstPtr vcloud, double voxel_leaf_size, double measurement_range)
	{
		PointCloud<PointXYZ>::Ptr filteredGridCLoud(new PointCloud<PointXYZ>);

		assert(voxel_leaf_size>=0.1);
		pcl::VoxelGrid<pcl::PointXYZ> voxel_grid_filter;
		voxel_grid_filter.setLeafSize(voxel_leaf_size, voxel_leaf_size, voxel_leaf_size);
		voxel_grid_filter.setInputCloud(vcloud);
		voxel_grid_filter.filter(*filteredGridCLoud);

		return filteredGridCLoud;
	}


protected:
	boost::shared_ptr<velodyne_rawdata::RawData> data_;

	template<class PointT>
	static
	PointCloud<PointXYZ>::Ptr
	convertToExternal (const PointCloud<PointT> &cloudSrc)
	{
		const int w=cloudSrc.width, h=cloudSrc.height;
		PointCloud<PointXYZ>::Ptr cloudExt (new PointCloud<PointXYZ>(w*h, 1));

		if (h==1) for (int i=0; i<w; ++i) {
			cloudExt->at(i).x = cloudSrc.at(i).x;
			cloudExt->at(i).y = cloudSrc.at(i).y;
			cloudExt->at(i).z = cloudSrc.at(i).z;
		}

		else for (int i=0; i<w; ++i) {
			for (int j=0; j<h; ++j) {
				cloudExt->at(i*w + j).x = cloudSrc.at(j, i).x;
				cloudExt->at(i*w + j).y = cloudSrc.at(j, i).y;
				cloudExt->at(i*w + j).z = cloudSrc.at(j, i).z;
			}
		}

		return cloudExt;
	}
};


void
createTrajectoryFromNDT (RandomAccessBag &bagsrc, Trajectory &resultTrack, const Trajectory &gnssTrack)
{
	if (bagsrc.getTopic() != "/velodyne_packets")
		throw runtime_error("Not Velodyne bag");

	VelodynePreprocessor VP(paramFileTest);
	NdtLocalizer lidarLocalizer(NuInitialConfig);

	bool initialized=false;

	// XXX: How to catch NDT's failure ?
	for (uint32_t ip=0; ip<bagsrc.size(); ++ip) {

		Pose cNdtPose;
		auto cMsg = bagsrc.at<velodyne_msgs::VelodyneScan>(ip);
		auto clx = VP.convertMessage(cMsg);

		if (!initialized) {
			try {

				auto cGnssPos = gnssTrack.at(cMsg->header.stamp);
				lidarLocalizer.putEstimation(cGnssPos);
				cNdtPose = lidarLocalizer.localize(clx);
				initialized = true;

			} catch (out_of_range &e) {
				continue;
			}
		}

		else {
			cNdtPose = lidarLocalizer.localize(clx);
		}

		PoseTimestamp tpose (cNdtPose);
		tpose.timestamp = cMsg->header.stamp;
		cerr << tpose.position().x() << " " <<
				tpose.position().y() << " " <<
				tpose.position().z() << endl;
		resultTrack.push_back(tpose);
	}

	return;
}
