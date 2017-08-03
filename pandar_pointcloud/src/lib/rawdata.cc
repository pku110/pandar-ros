/*
 *  Copyright (C) 2007 Austin Robot Technology, Patrick Beeson
 *  Copyright (C) 2009, 2010, 2012 Austin Robot Technology, Jack O'Quin
 *  Copyright (c) 2017 Hesai Photonics Technology, Yang Sheng
 *
 *  License: Modified BSD Software License Agreement
 *
 *  $Id$
 */

/**
 *  @file
 *
 *  Pandar40 3D LIDAR data accessor class implementation.
 *
 *  Class for unpacking raw Pandar40 LIDAR packets into useful
 *  formats.
 *
 *  Derived classes accept raw Pandar40 data for either single packets
 *  or entire rotations, and provide it in various formats for either
 *  on-line or off-line processing.
 *
 *  @author Patrick Beeson
 *  @author Jack O'Quin
 *  @author Yang Sheng
 *
 */

#include <fstream>
#include <math.h>

#include <ros/ros.h>
#include <ros/package.h>
#include <angles/angles.h>

#include <pandar_pointcloud/rawdata.h>

namespace pandar_rawdata
{
////////////////////////////////////////////////////////////////////////
//
// RawData base class implementation
//
////////////////////////////////////////////////////////////////////////

RawData::RawData()
{}

/** Update parameters: conversions and update */
void RawData::setParameters(double min_range,
                            double max_range,
                            double view_direction,
                            double view_width)
{
    config_.min_range = min_range;
    config_.max_range = max_range;

	//TODO: YS: not support view angle setting currently.
    //converting angle parameters into the pandar reference (rad)
    config_.tmp_min_angle = view_direction + view_width/2;
    config_.tmp_max_angle = view_direction - view_width/2;

    //computing positive modulo to keep theses angles into [0;2*M_PI]
    config_.tmp_min_angle = fmod(fmod(config_.tmp_min_angle,2*M_PI) + 2*M_PI,2*M_PI);
    config_.tmp_max_angle = fmod(fmod(config_.tmp_max_angle,2*M_PI) + 2*M_PI,2*M_PI);

    //converting into the hardware pandar ref (negative yaml and degrees)
    //adding 0.5 perfomrs a centered double to int conversion
    config_.min_angle = 100 * (2*M_PI - config_.tmp_min_angle) * 180 / M_PI + 0.5;
    config_.max_angle = 100 * (2*M_PI - config_.tmp_max_angle) * 180 / M_PI + 0.5;
    if (config_.min_angle == config_.max_angle)
    {
        //avoid returning empty cloud if min_angle = max_angle
        config_.min_angle = 0;
        config_.max_angle = 36000;
    }
}

/** Set up for on-line operation. */
int RawData::setup(ros::NodeHandle private_nh)
{
    // get path to angles.config file for this device
    if (!private_nh.getParam("calibration", config_.calibrationFile))
    {
        ROS_ERROR_STREAM("No calibration angles specified! Using default values!");

        std::string pkgPath = ros::package::getPath("pandar_pointcloud");
        config_.calibrationFile = pkgPath + "/params/Lidar-Correction-18.csv";
    }

    ROS_INFO_STREAM("correction angles: " << config_.calibrationFile);

    calibration_.read(config_.calibrationFile);
    if (!calibration_.initialized) {
        ROS_ERROR_STREAM("Unable to open calibration file: " <<
                         config_.calibrationFile);
        return -1;
    }

    ROS_INFO_STREAM("Number of lasers: " << calibration_.num_lasers << ".");

    // Set up cached values for sin and cos of all the possible headings
    for (uint16_t rot_index = 0; rot_index < ROTATION_MAX_UNITS; ++rot_index) {
        float rotation = angles::from_degrees(ROTATION_RESOLUTION * rot_index);
        cos_lookup_table_[rot_index] = cosf(rotation);
        sin_lookup_table_[rot_index] = sinf(rotation);
    }
    return 0;
}


/** Set up for offline operation */
int RawData::setupOffline(std::string calibration_file, double max_range_, double min_range_)
{

    config_.max_range = max_range_;
    config_.min_range = min_range_;
    ROS_INFO_STREAM("data ranges to publish: ["
                    << config_.min_range << ", "
                    << config_.max_range << "]");

    config_.calibrationFile = calibration_file;

    ROS_INFO_STREAM("correction angles: " << config_.calibrationFile);

    calibration_.read(config_.calibrationFile);
    if (!calibration_.initialized) {
        ROS_ERROR_STREAM("Unable to open calibration file: " <<
                         config_.calibrationFile);
        return -1;
    }

    // Set up cached values for sin and cos of all the possible headings
    for (uint16_t rot_index = 0; rot_index < ROTATION_MAX_UNITS; ++rot_index) {
        float rotation = angles::from_degrees(ROTATION_RESOLUTION * rot_index);
        cos_lookup_table_[rot_index] = cosf(rotation);
        sin_lookup_table_[rot_index] = sinf(rotation);
    }
    return 0;
}

int RawData::parseRawData(raw_packet_t* packet, const uint8_t* buf, const int len)
{
    if(len != PACKET_SIZE) {
		ROS_WARN_STREAM("packet size mismatch!");
        return -1;
	}

    int index = 0;
    // 6x BLOCKs
    for(int i = 0 ; i < BLOCKS_PER_PACKET ; i++) {
		raw_block_t& block = packet->blocks[i];
        block.sob = (buf[index] & 0xff)| ((buf[index + 1] & 0xff)<< 8);
        block.azimuth = (buf[index + 2]& 0xff) | ((buf[index + 3]& 0xff) << 8);
        index += SOB_ANGLE_SIZE;
        // 40x measures
        for(int j = 0 ; j < LASER_COUNT ; j++) {
			raw_measure_t& measure = block.measures[j];
            measure.range = (buf[index]& 0xff)
				| ((buf[index + 1]& 0xff) << 8)
				| ((buf[index + 2]& 0xff) << 16 );
            measure.reflectivity = (buf[index + 3]& 0xff)
				| ((buf[index + 4]& 0xff) << 8);

            // TODO: Filtering wrong data for LiDAR Bugs.
            if((measure.range == 0x010101 && measure.reflectivity == 0x0101)
					|| measure.range > (200 * 1000 /2 /* 200m -> 2mm */)) {
                measure.range = 0;
                measure.reflectivity = 0;
            }
            index += RAW_MEASURE_SIZE;
        }
    }

    index += RESERVE_SIZE; // skip reserved bytes

    packet->revolution = (buf[index]& 0xff)| (buf[index + 1]& 0xff) << 8;
    index += REVOLUTION_SIZE;

    packet->timestamp = (buf[index]& 0xff)| (buf[index + 1]& 0xff) << 8 |
                        ((buf[index + 2 ]& 0xff) << 16) | ((buf[index + 3]& 0xff) << 24);
    index += TIMESTAMP_SIZE;
    packet->factory[0] = buf[index]& 0xff;
    packet->factory[1] = buf[index + 1]& 0xff;
    index += FACTORY_ID_SIZE;
    return 0;
}

void RawData::computeXYZIR(PPoint& point, int azimuth,
		const raw_measure_t& laserReturn, const pandar_pointcloud::PandarLaserCorrection& correction)
{
    double cos_azimuth, sin_azimuth;
    double distanceM = laserReturn.range * 0.002;

    point.intensity = static_cast<float> (laserReturn.reflectivity >> 8);
    if (distanceM < config_.min_range || distanceM > config_.max_range)
    {
        point.x = point.y = point.z = std::numeric_limits<float>::quiet_NaN ();
        return;
    }
    if (correction.azimuthCorrection == 0)
    {
        cos_azimuth = cos_lookup_table_[azimuth];
        sin_azimuth = sin_lookup_table_[azimuth];
    }
    else
    {
        double azimuthInRadians = angles::from_degrees( (static_cast<double> (azimuth) / 100.0) + correction.azimuthCorrection);
        cos_azimuth = std::cos (azimuthInRadians);
        sin_azimuth = std::sin (azimuthInRadians);
    }

    distanceM += correction.distanceCorrection;

    double xyDistance = distanceM * correction.cosVertCorrection;

    point.x = static_cast<float> (xyDistance * sin_azimuth - correction.horizontalOffsetCorrection * cos_azimuth);
    point.y = static_cast<float> (xyDistance * cos_azimuth + correction.horizontalOffsetCorrection * sin_azimuth);
    point.z = static_cast<float> (distanceM * correction.sinVertCorrection + correction.verticalOffsetCorrection);
    if (point.x == 0 && point.y == 0 && point.z == 0)
    {
        point.x = point.y = point.z = std::numeric_limits<float>::quiet_NaN ();
    }
}

void RawData::toPointClouds (raw_packet_t* packet, PPointCloud& pc)
{
    for (int i = 0; i < BLOCKS_PER_PACKET; i++) {
		const raw_block_t& firing_data = packet->blocks[i];

        for (int j = 0; j < LASER_COUNT; j++) {
            PPoint xyzir;
            computeXYZIR (xyzir, firing_data.azimuth,
					firing_data.measures[j], calibration_.laser_corrections[j]);
            if (pcl_isnan (xyzir.x) || pcl_isnan (xyzir.y) || pcl_isnan (xyzir.z))
            {
                continue;
            }
			xyzir.ring = j;
			pc.points.push_back(xyzir);
			pc.width++;
        }
    }
}

/** @brief convert raw packet to point cloud
 *
 *  @param pkt raw packet to unpack
 *  @param pc shared pointer to point cloud (points are appended)
 */
void RawData::unpack(const pandar_msgs::PandarPacket &pkt, PPointCloud &pc)
{
    ROS_DEBUG_STREAM("Received packet, time: " << pkt.stamp);

	raw_packet_t packet;
	parseRawData(&packet, &pkt.data[0], pkt.data.size());
	toPointClouds(&packet, pc);
}

} // namespace pandar_rawdata