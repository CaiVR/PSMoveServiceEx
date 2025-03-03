//-- includes -----
#include "ServerControllerView.h"

#include "AtomicPrimitives.h"
#include "BluetoothRequests.h"
#include "ControllerManager.h"
#include "DeviceManager.h"
#include "MathAlignment.h"
#include "ServerLog.h"
#include "ServerRequestHandler.h"
#include "CompoundPoseFilter.h"
#include "KalmanPoseFilter.h"
#include "PSDualShock4Controller.h"
#include "PSMoveController.h"
#include "PSNaviController.h"
#include "VirtualController.h"
#include "PSMoveProtocolInterface.h"
#include "PSMoveProtocol.pb.h"
#include "ServerUtility.h"
#include "ServerTrackerView.h"

#include <glm/glm.hpp>

//-- typedefs ----
using t_high_resolution_timepoint= std::chrono::time_point<std::chrono::high_resolution_clock>;
using t_high_resolution_duration= t_high_resolution_timepoint::duration;

//-- constants -----
static const float k_min_time_delta_seconds = 1 / 2500.f;
static const float k_max_time_delta_seconds = 1 / 30.f;

//-- macros -----
#define SET_BUTTON_BIT(bitmask, bit_index, button_state) \
    bitmask|= (button_state == CommonControllerState::Button_DOWN || button_state == CommonControllerState::Button_PRESSED) ? (0x1 << (bit_index)) : 0x0;

//-- private methods -----
static IPoseFilter *pose_filter_factory(
    const CommonDeviceState::eDeviceType deviceType,
    const std::string &position_filter_type,
    const std::string &orientation_filter_type,
    const PoseFilterConstants &constants);

static void init_filters_for_psmove(
    const PSMoveController *psmoveController, 
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter);
static void init_filters_for_psdualshock4(
    const PSDualShock4Controller *psdualshock4Controller,
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter);
static void init_filters_for_virtual_controller(
    const VirtualController *psmoveController, 
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter);

static void post_imu_filter_packets_for_psmove(
    const PSMoveController *psmove,
	const PSMoveControllerInputState *psmoveState,
    const t_high_resolution_timepoint now, 
	const t_high_resolution_duration secondsSinceLastUpdate,
	t_controller_pose_sensor_queue *pose_filter_queue);
static void post_optical_filter_packet_for_psmove(
    const PSMoveController *psmove,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *poseEstimation,
	t_controller_pose_optical_queue *pose_filter_queue);

static void post_imu_filter_packets_for_ds4(
    const PSDualShock4Controller *ds4, 
	const DualShock4ControllerInputState *psmoveState,
    const t_high_resolution_timepoint now, 
	const t_high_resolution_duration secondsSinceLastUpdate,
	t_controller_pose_sensor_queue *pose_filter_queue);
static void post_optical_filter_packet_for_ds4(
    const PSDualShock4Controller *ds4,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *poseEstimation,
	t_controller_pose_optical_queue *pose_filter_queue);
static void post_imu_filter_packets_for_virtual_controller(
	const VirtualController *psmove,
	const VirtualControllerState *psmoveState,
	const t_high_resolution_timepoint now,
	const t_high_resolution_duration duration_since_last_update,
	t_controller_pose_sensor_queue *pose_filter_queue);

static void post_optical_filter_packet_for_virtual_controller(
    const VirtualController *ds4,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *poseEstimation,
	t_controller_pose_optical_queue *pose_filter_queue);

static void generate_psmove_data_frame_for_stream(
    const ServerControllerView *controller_view, const ControllerStreamInfo *stream_info, PSMoveProtocol::DeviceOutputDataFrame *data_frame);
static void generate_psnavi_data_frame_for_stream(
    const ServerControllerView *controller_view, const ControllerStreamInfo *stream_info, PSMoveProtocol::DeviceOutputDataFrame *data_frame);
static void generate_psdualshock4_data_frame_for_stream(
    const ServerControllerView *controller_view, const ControllerStreamInfo *stream_info, PSMoveProtocol::DeviceOutputDataFrame *data_frame);
static void generate_virtual_controller_data_frame_for_stream(
    const ServerControllerView *controller_view, const ControllerStreamInfo *stream_info, PSMoveProtocol::DeviceOutputDataFrame *data_frame);

static void computeSpherePoseForControllerFromSingleTracker(
    const ServerControllerView *controllerView,
    const ServerTrackerViewPtr tracker,
    ControllerOpticalPoseEstimation *tracker_pose_estimation,
    ControllerOpticalPoseEstimation *multicam_pose_estimation);
static void computeLightBarPoseForControllerFromSingleTracker(
    const ServerControllerView *controllerView,
    const ServerTrackerViewPtr tracker,
    ControllerOpticalPoseEstimation *tracker_pose_estimation,
    ControllerOpticalPoseEstimation *multicam_pose_estimation);
static void computeSpherePoseForControllerFromMultipleTrackers(
    const ServerControllerView *controllerView,
    const TrackerManager* tracker_manager,
    const int *valid_projection_tracker_ids,
    const int projections_found,
    ControllerOpticalPoseEstimation *tracker_pose_estimations,
    ControllerOpticalPoseEstimation *multicam_pose_estimation);
static void computeLightBarPoseForControllerFromMultipleTrackers(
    const ServerControllerView *controllerView,
    const TrackerManager* tracker_manager,
    const int *valid_projection_tracker_ids,
    const int projections_found,
    ControllerOpticalPoseEstimation *tracker_pose_estimations,
    ControllerOpticalPoseEstimation *multicam_pose_estimation);

//-- public implementation -----
ServerControllerView::ServerControllerView(const int device_id)
    : ServerDeviceView(device_id)
    , m_tracking_listener_count(0)
    , m_tracking_enabled(false)
    , m_roi_disable_count(0)
    , m_LED_override_active(false)
    , m_device(nullptr)
    , m_tracker_pose_estimations(nullptr)
    , m_multicam_pose_estimation(nullptr)
    , m_pose_filter(nullptr)
    , m_pose_filter_space(nullptr)
    , m_lastPollSeqNumProcessed(-1)
    , m_last_filter_update_timestamp()
    , m_last_filter_update_timestamp_valid(false)
{
    m_tracking_color = std::make_tuple(0x00, 0x00, 0x00);
    m_LED_override_color = std::make_tuple(0x00, 0x00, 0x00);
}

ServerControllerView::~ServerControllerView()
{
}

bool ServerControllerView::allocate_device_interface(
    const class DeviceEnumerator *enumerator)
{
    switch (enumerator->get_device_type())
    {
    case CommonDeviceState::PSMove:
        {
            m_device = new PSMoveController();
			m_device->setControllerListener(this); // Listen for IMU packets

            m_tracker_pose_estimations = new ControllerOpticalPoseEstimation[TrackerManager::k_max_devices];
            m_pose_filter= nullptr; // no pose filter until the device is opened

            for (int tracker_index = 0; tracker_index < TrackerManager::k_max_devices; ++tracker_index)
            {
                m_tracker_pose_estimations[tracker_index].clear();
            }

            m_multicam_pose_estimation = new ControllerOpticalPoseEstimation();
            m_multicam_pose_estimation->clear();
        } break;
    case CommonDeviceState::PSNavi:
        {
            m_device= new PSNaviController();
            m_pose_filter= nullptr;
            m_multicam_pose_estimation = nullptr;
        } break;
    case CommonDeviceState::PSDualShock4:
        {
            m_device = new PSDualShock4Controller();
			m_device->setControllerListener(this); // Listen for IMU packets

            m_tracker_pose_estimations = new ControllerOpticalPoseEstimation[TrackerManager::k_max_devices];
            m_pose_filter = nullptr; // no pose filter until the device is opened

            for (int tracker_index = 0; tracker_index < TrackerManager::k_max_devices; ++tracker_index)
            {
                m_tracker_pose_estimations[tracker_index].clear();
            }

            m_multicam_pose_estimation = new ControllerOpticalPoseEstimation();
            m_multicam_pose_estimation->clear();
        } break;
    case CommonDeviceState::VirtualController:
        {
            m_device = new VirtualController();
			m_device->setControllerListener(this); // Enforce updates for virtual controllers. Send fake IMU packets.

            m_tracker_pose_estimations = new ControllerOpticalPoseEstimation[TrackerManager::k_max_devices];
            m_pose_filter = nullptr; // no pose filter until the device is opened

            for (int tracker_index = 0; tracker_index < TrackerManager::k_max_devices; ++tracker_index)
            {
                m_tracker_pose_estimations[tracker_index].clear();
            }

            m_multicam_pose_estimation = new ControllerOpticalPoseEstimation();
            m_multicam_pose_estimation->clear();
        } break;
    default:
        break;
    }

    return m_device != nullptr;
}

void ServerControllerView::free_device_interface()
{
    if (m_multicam_pose_estimation != nullptr)
    {
        delete m_multicam_pose_estimation;
        m_multicam_pose_estimation= nullptr;
    }

    if (m_tracker_pose_estimations != nullptr)
    {
        delete[] m_tracker_pose_estimations;
        m_tracker_pose_estimations = nullptr;
    }

    if (m_pose_filter != nullptr)
    {
        delete m_pose_filter;
        m_pose_filter= nullptr;
    }

    if (m_device != nullptr)
    {
        delete m_device;  // Deleting abstract object should be OK because
                          // this (ServerDeviceView) is abstract as well.
                          // All non-abstract children will have non-abstract types
                          // for m_device.
        m_device= nullptr;
    }
}

bool ServerControllerView::open(const class DeviceEnumerator *enumerator)
{
    // Attempt to open the controller
    bool bSuccess= ServerDeviceView::open(enumerator);
    bool bAllocateTrackingColor = false;

    // Setup the orientation filter based on the controller configuration
    if (bSuccess)
    {
        IDeviceInterface *device= getDevice();

        switch (device->getDeviceType())
        {
        case CommonDeviceState::PSMove:
            {
                const PSMoveController *psmoveController= this->castCheckedConst<PSMoveController>();

                // Don't bother initializing any filters or allocating a tracking color
                // for usb connected controllers
                if (psmoveController->getIsBluetooth())
                {
                    // Create a pose filter based on the controller type
                    resetPoseFilter();
                    m_multicam_pose_estimation->clear();

                    bAllocateTrackingColor = true;
                }
            } break;
        case CommonDeviceState::PSNavi:
            // No orientation filter for the navi
            assert(m_pose_filter == nullptr);
            break;
        case CommonDeviceState::PSDualShock4:
            {
                const PSDualShock4Controller *psdualshock4Controller = this->castCheckedConst<PSDualShock4Controller>();

                // Don't bother initializing any filters or allocating a tracking color
                // for usb connected controllers
                if (psdualshock4Controller->getIsBluetooth())
                {
                    // Create a pose filter based on the controller type
                    resetPoseFilter();
                    m_multicam_pose_estimation->clear();

                    bAllocateTrackingColor = true;
                }
            } break;
        case CommonDeviceState::VirtualController:
            {
                const VirtualController *virtualController = this->castCheckedConst<VirtualController>();

                // Create a pose filter based on the controller type
                resetPoseFilter();
                m_multicam_pose_estimation->clear();

                bAllocateTrackingColor = true;
            } break;
        default:
            break;
        }

        // Reset the poll sequence number high water mark
        m_lastPollSeqNumProcessed= -1;
    }

    // If needed for this kind of controller, assign a tracking color id
    if (bAllocateTrackingColor)
    {
        eCommonTrackingColorID tracking_color_id;
        assert(m_device != nullptr);

        // If this device already has a valid tracked color assigned, 
        // claim it from the pool (or another controller that had it previously)
        if (m_device->getTrackingColorID(tracking_color_id) && tracking_color_id != eCommonTrackingColorID::INVALID_COLOR)
        {
            DeviceManager::getInstance()->m_tracker_manager->claimTrackingColorID(this, tracking_color_id);
        }
        else
        {
            // Allocate a color from the list of remaining available color ids
            eCommonTrackingColorID allocatedColorID= DeviceManager::getInstance()->m_tracker_manager->allocateTrackingColorID();

            // Attempt to assign the tracking color id to the controller
            if (!m_device->setTrackingColorID(allocatedColorID))
            {
                // If the device can't be assigned a tracking color, release the color back to the pool
                DeviceManager::getInstance()->m_tracker_manager->freeTrackingColorID(allocatedColorID);
            }
        }
    }

    // Clear the filter update timestamp
    m_last_filter_update_timestamp = std::chrono::time_point<std::chrono::high_resolution_clock>();
    m_last_filter_update_timestamp_valid= false;

    return bSuccess;
}

void ServerControllerView::close()
{
    set_tracking_enabled_internal(false);

    eCommonTrackingColorID tracking_color_id= eCommonTrackingColorID::INVALID_COLOR;
    if (m_device != nullptr && m_device->getTrackingColorID(tracking_color_id))
    {
        if (tracking_color_id != eCommonTrackingColorID::INVALID_COLOR)
        {
            DeviceManager::getInstance()->m_tracker_manager->freeTrackingColorID(tracking_color_id);
        }
    }

    ServerDeviceView::close();
}

bool ServerControllerView::recenterOrientation(const CommonDeviceQuaternion& q_pose_relative_to_identity_pose)
{
    bool bSuccess = false;
    IPoseFilter *filter = getPoseFilterMutable();

    if (filter != nullptr)
    {
        // Get the pose that we expect the controller to be in (relative to the pose it's in by default).
        // For example, the psmove controller's default mesh has it laying flat,
        // but when we call reset_orientation in the HMD alignment tool, we expect the controller is pointing up.
        const Eigen::Quaternionf q_pose(
            q_pose_relative_to_identity_pose.w,
            q_pose_relative_to_identity_pose.x,
            q_pose_relative_to_identity_pose.y,
            q_pose_relative_to_identity_pose.z);

        // Get the rotation that would align the global +X axis with the global forward direction
        const float global_forward_degrees = DeviceManager::getInstance()->m_tracker_manager->getConfig().global_forward_degrees;
        const float global_forward_radians = global_forward_degrees * k_degrees_to_radians;
        const Eigen::EulerAnglesf global_forward_euler(Eigen::Vector3f(0.f, global_forward_radians, 0.f));
        const Eigen::Quaternionf global_forward_quat = eigen_euler_angles_to_quaternionf(global_forward_euler);

        // Get the rotation that would align the global +X axis with the controller identity forward
        const float controller_forward_degrees = m_device->getIdentityForwardDegrees();
        const float controller_forward_radians = global_forward_degrees * k_degrees_to_radians;
        const Eigen::EulerAnglesf controller_forward_euler(Eigen::Vector3f(0.f, controller_forward_radians, 0.f));
        const Eigen::Quaternionf controller_forward_quat = eigen_euler_angles_to_quaternionf(controller_forward_euler);

        // Compute the relative rotation from global forward to controller identity forward.
        // If the controllers identity forward and global forward are the same this will cancel out.
        // Usually both are -Z.
        const Eigen::Quaternionf identity_pose_relative_to_global_forward= 
            eigen_quaternion_concatenate(global_forward_quat.conjugate(), controller_forward_quat);

        // Compute the pose that the controller claims to be in relative to global forward.
        // For the normal re-centering case q_pose_relative_to_identity_pose is the identity quaternion.
        const Eigen::Quaternionf controller_pose_relative_to_global_forward=
            eigen_quaternion_concatenate(q_pose, identity_pose_relative_to_global_forward);

        // Tell the pose filter that the orientation state should now be relative to controller_pose_relative_to_global_forward
        filter->recenterOrientation(controller_pose_relative_to_global_forward);
        bSuccess = true;
    }

    return bSuccess;
}

void ServerControllerView::resetPoseFilter()
{
    assert(m_device != nullptr);

    if (m_pose_filter != nullptr)
    {
        delete m_pose_filter;
        m_pose_filter = nullptr;
    }

    if (m_pose_filter_space != nullptr)
    {
        delete m_pose_filter_space;
        m_pose_filter_space = nullptr;
    }

    switch (m_device->getDeviceType())
    {
    case CommonDeviceState::PSMove:
        {
            init_filters_for_psmove(
                static_cast<PSMoveController *>(m_device),
                &m_pose_filter_space, &m_pose_filter);
        } break;
    case CommonDeviceState::PSDualShock4:
        {
            init_filters_for_psdualshock4(
                static_cast<PSDualShock4Controller *>(m_device),
                &m_pose_filter_space, &m_pose_filter);
        } break;
    case CommonDeviceState::VirtualController:
        {
            init_filters_for_virtual_controller(
                static_cast<VirtualController *>(m_device),
                &m_pose_filter_space, &m_pose_filter);
        } break;
	case CommonDeviceState::PSNavi:
		// No pose filter
		break;
    default:
        assert(false && "unreachable");
    }
}

void ServerControllerView::updateOpticalPoseEstimation(TrackerManager* tracker_manager)
{
    const std::chrono::time_point<std::chrono::high_resolution_clock> now= std::chrono::high_resolution_clock::now();
	const TrackerManagerConfig &trackerMgrConfig = DeviceManager::getInstance()->m_tracker_manager->getConfig();

    // TODO: Probably need to first update IMU state to get velocity.
    // If velocity is too high, don't bother getting a new position.
    // Though it may be enough to just use the camera ROI as the limit.
    
    if (getIsTrackingEnabled() && getControllerOpticalTrackingEnabled())
    {
        int valid_projection_tracker_ids[TrackerManager::k_max_devices];
        int projections_found = 0;

		int available_trackers = 0;

		static bool occluded_tracker_ids[TrackerManager::k_max_devices][ControllerManager::k_max_devices];
		static float occluded_projection_tracker_ids[TrackerManager::k_max_devices][ControllerManager::k_max_devices][2];

        CommonDeviceTrackingShape trackingShape;
        m_device->getTrackingShape(trackingShape);
        assert(trackingShape.shape_type != eCommonTrackingShapeType::INVALID_SHAPE);

		struct projectionInfo
		{
			int tracker_id;
			float screen_area;
		};
		std::vector<projectionInfo> sorted_projections;

		for (int tracker_id = 0; tracker_id < tracker_manager->getMaxDevices(); ++tracker_id)
		{
			const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
			if (tracker->getIsOpen())
			{
				ControllerOpticalPoseEstimation &trackerPoseEstimateRef = m_tracker_pose_estimations[tracker_id];

				projectionInfo info;
				info.tracker_id = tracker_id;
				info.screen_area = trackerPoseEstimateRef.projection.screen_area;
				sorted_projections.push_back(info);
			}
		}

		// Sort by biggest projector.
		// Go through all trackers and sort them by biggest projector to make tracking quality better.
		// The bigger projections should be closer to trackers and smaller far away.
		std::sort(
			sorted_projections.begin(), sorted_projections.end(),
			[](const projectionInfo & a, const projectionInfo & b) -> bool
		{
			return a.screen_area > b.screen_area;
		});


        // Find the projection of the controller from the perspective of each tracker.
        // In the case of sphere projections, go ahead and compute the tracker relative position as well.
        for (int list_index = 0; list_index < sorted_projections.size(); ++list_index)
        {
			int tracker_id = sorted_projections[list_index].tracker_id;

            ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
            ControllerOpticalPoseEstimation &trackerPoseEstimateRef = m_tracker_pose_estimations[tracker_id];

			const bool bWasTracking = trackerPoseEstimateRef.bCurrentlyTracking;

            // Assume we're going to lose tracking this frame
            bool bCurrentlyTracking = false;
			bool bOccluded = false;

            if (tracker->getIsOpen())
            {
				available_trackers++;

                // See how long it's been since we got a new video frame
                const std::chrono::time_point<std::chrono::high_resolution_clock> now= 
                    std::chrono::high_resolution_clock::now();
                const std::chrono::duration<float, std::milli> timeSinceNewDataMillis= 
                    now - tracker->getLastNewDataTimestamp();
                const float timeoutMilli= 
                    static_cast<float>(trackerMgrConfig.optical_tracking_timeout);

                // Can't compute tracking on video data that's too old
                if (timeSinceNewDataMillis.count() < timeoutMilli)
                {
                    // Initially the newTrackerPoseEstimate is a copy of the existing pose
                    bool bIsVisibleThisUpdate= false;

					// If a new video frame is available this tick, 
					// attempt to update the tracking location
					if (tracker->getHasUnpublishedState())
					{
						// Create a copy of the pose estimate state so that in event of a 
						// failure part way through computing the projection we don't
						// set partially valid state
						ControllerOpticalPoseEstimation newTrackerPoseEstimate = trackerPoseEstimateRef;

						if (tracker->computeProjectionForController(
								this, 
								&trackingShape,
								&newTrackerPoseEstimate))
						{
							bIsVisibleThisUpdate= true;

							// Actually apply the pose estimate state
							trackerPoseEstimateRef= newTrackerPoseEstimate;
							trackerPoseEstimateRef.last_visible_timestamp = now;
						}
					}

					bool bIsOccluded = false;

					//Create an occlusion area at the last seen valid tracked projection.
					//If the projection center is near the occluded area it will not mark the projection as valid.
					//This will remove jitter when the shape of the controllers is partially visible to the trackers.
					if (trackerMgrConfig.occluded_area_on_loss_size >= 0.01)
					{
						int controller_id = this->getDeviceID();

						if (!occluded_tracker_ids[tracker_id][controller_id])
						{
							if (bWasTracking || bIsVisibleThisUpdate)
							{
								occluded_tracker_ids[tracker_id][controller_id] = false;
								occluded_projection_tracker_ids[tracker_id][controller_id][0] = trackerPoseEstimateRef.projection.shape.ellipse.center.x;
								occluded_projection_tracker_ids[tracker_id][controller_id][1] = trackerPoseEstimateRef.projection.shape.ellipse.center.y;
							}
							else
							{
								occluded_tracker_ids[tracker_id][controller_id] = true;
							}
						}

						if (occluded_tracker_ids[tracker_id][controller_id])
						{
							if (bWasTracking || bIsVisibleThisUpdate)
							{
								bool bInArea = (abs(trackerPoseEstimateRef.projection.shape.ellipse.center.x - occluded_projection_tracker_ids[tracker_id][controller_id][0])
													< trackerMgrConfig.occluded_area_on_loss_size
												&& abs(trackerPoseEstimateRef.projection.shape.ellipse.center.y - occluded_projection_tracker_ids[tracker_id][controller_id][1])
													< trackerMgrConfig.occluded_area_on_loss_size);
								
								bool bRegain = (fmaxf(trackerPoseEstimateRef.projection.screen_area, trackerMgrConfig.min_valid_projection_area) 
													> trackerMgrConfig.occluded_area_regain_projection_size);

								if (bInArea && !bRegain)
								{
									bIsOccluded = true;

									trackerPoseEstimateRef.occlusionAreaSize = trackerMgrConfig.occluded_area_on_loss_size;
									trackerPoseEstimateRef.occlusionAreaPos.x = occluded_projection_tracker_ids[tracker_id][controller_id][0];
									trackerPoseEstimateRef.occlusionAreaPos.y = occluded_projection_tracker_ids[tracker_id][controller_id][1];
								}
								else
								{
									occluded_tracker_ids[tracker_id][controller_id] = false;
								}
							}
						}
					}

					// Ignore projections that are occluded BUT always pass atleast 2 biggest projected trackers.
					if (!bIsOccluded || projections_found < trackerMgrConfig.occluded_area_ignore_trackers)
					{
						bOccluded = false;

						// If the projection isn't too old (or updated this tick), 
						// say we have a valid tracked location
						if ((bWasTracking && !tracker->getHasUnpublishedState()) || bIsVisibleThisUpdate)
						{
							// If this tracker has a valid projection for the controller
							// add it to the tracker id list
							valid_projection_tracker_ids[projections_found] = tracker_id;
							++projections_found;

							// Flag this pose estimate as invalid
							bCurrentlyTracking = true;
						}
					}
					else
					{
						bOccluded = true;
					}
                }
            }

            // Keep track of the last time the position estimate was updated
            trackerPoseEstimateRef.last_update_timestamp = now;
            trackerPoseEstimateRef.bValidTimestamps = true;
            trackerPoseEstimateRef.bCurrentlyTracking = bCurrentlyTracking;
			trackerPoseEstimateRef.bIsOccluded = bOccluded;
        }

        // How we compute the final world pose estimate varies based on
        // * Number of trackers that currently have a valid projections of the controller
        // * The kind of projection shape (psmove sphere or ds4 lightbar)
        if (projections_found > 1)
        {
            // If multiple trackers can see the controller, 
            // triangulate all pairs of projections and average the results
            switch (trackingShape.shape_type)
            {
            case eCommonTrackingShapeType::Sphere:
                computeSpherePoseForControllerFromMultipleTrackers(
                    this,
                    tracker_manager,
                    valid_projection_tracker_ids,
                    projections_found,
                    m_tracker_pose_estimations,
                    m_multicam_pose_estimation);
                break;
            case eCommonTrackingShapeType::LightBar:
                computeLightBarPoseForControllerFromMultipleTrackers(
                    this,
                    tracker_manager,
                    valid_projection_tracker_ids,
                    projections_found,
                    m_tracker_pose_estimations,
                    m_multicam_pose_estimation);
                break;
            default:
                assert(false && "unreachable");
            }
        }
        else if (projections_found == 1 && (available_trackers == 1 || !trackerMgrConfig.ignore_pose_from_one_tracker))
        {
            const int tracker_id = valid_projection_tracker_ids[0];
            const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);

            // If only one tracker can see the controller, 
            // then use the tracker to derive a world space location
            switch (trackingShape.shape_type)
            {
            case eCommonTrackingShapeType::Sphere:
                computeSpherePoseForControllerFromSingleTracker(
                    this,
                    tracker,
                    &m_tracker_pose_estimations[tracker_id],
                    m_multicam_pose_estimation);
                break;
            case eCommonTrackingShapeType::LightBar:
                computeLightBarPoseForControllerFromSingleTracker(
                    this,
                    tracker,
                    &m_tracker_pose_estimations[tracker_id],
                    m_multicam_pose_estimation);
                break;
            default:
                assert(false && "unreachable");
            }
        }
        // If no trackers can see the controller, maintain the last known position and time it was seen
        else
        {
            m_multicam_pose_estimation->bCurrentlyTracking= false;
        }

        // Update the position estimation timestamps
        if (m_multicam_pose_estimation->bCurrentlyTracking)
        {
            m_multicam_pose_estimation->last_visible_timestamp = now;
        }
        m_multicam_pose_estimation->last_update_timestamp = now;
        m_multicam_pose_estimation->bValidTimestamps = true;
    }

	// Update the filter if we have a valid optically tracked pose
	// TODO: These packets will eventually get posted from the notifyTrackerDataReceived()
	// callback function which will be called by camera processing threads as new video
	// frames are received.
	if (m_multicam_pose_estimation->bCurrentlyTracking)
	{
		switch (getControllerDeviceType())
		{
		case CommonDeviceState::PSMove:
			{
				const PSMoveController *psmove = this->castCheckedConst<PSMoveController>();

				post_optical_filter_packet_for_psmove(
					psmove,
					now,
					m_multicam_pose_estimation,
					&m_PoseSensorOpticalPacketQueue);
			} break;
		case CommonDeviceState::PSDualShock4:
			{
				const PSDualShock4Controller *ds4 = this->castCheckedConst<PSDualShock4Controller>();

				post_optical_filter_packet_for_ds4(
					ds4,
					now,
					m_multicam_pose_estimation,
					&m_PoseSensorOpticalPacketQueue);
			} break;
		case CommonDeviceState::VirtualController:
			{
				const VirtualController *virtual_controller = this->castCheckedConst<VirtualController>();

				post_optical_filter_packet_for_virtual_controller(
					virtual_controller,
					now,
					m_multicam_pose_estimation,
					&m_PoseSensorOpticalPacketQueue);
			} break;
		default:
			assert(0 && "Unhandled Controller Type");
		}
	}
}

void 
ServerControllerView::notifySensorDataReceived(const CommonDeviceState *sensor_state)
{
    // Compute the time in seconds since the last update
    const t_high_resolution_timepoint now = std::chrono::high_resolution_clock::now();
	t_high_resolution_duration durationSinceLastUpdate= t_high_resolution_duration::zero();

	if (m_bIsLastSensorDataTimestampValid)
	{
		durationSinceLastUpdate = now - m_lastSensorDataTimestamp;
	}
	m_lastSensorDataTimestamp= now;
	m_bIsLastSensorDataTimestampValid= true;

	// Apply device specific filtering
    switch (sensor_state->DeviceType)
    {
    case CommonDeviceState::PSMove:
        {
            const PSMoveController *psmove = this->castCheckedConst<PSMoveController>();
            const PSMoveControllerInputState *psmoveState = 
				static_cast<const PSMoveControllerInputState *>(sensor_state);

            // Only update the position filter when tracking is enabled
            post_imu_filter_packets_for_psmove(
                psmove, psmoveState,
                now, durationSinceLastUpdate,
				&m_PoseSensorIMUPacketQueue);
        } break;
	case CommonDeviceState::PSDualShock4:
	{
		const PSDualShock4Controller *ds4 = this->castCheckedConst<PSDualShock4Controller>();
		const DualShock4ControllerInputState *ds4State =
			static_cast<const DualShock4ControllerInputState *>(sensor_state);

		// Only update the position filter when tracking is enabled
		post_imu_filter_packets_for_ds4(
			ds4, ds4State,
			now, durationSinceLastUpdate,
			&m_PoseSensorIMUPacketQueue);
	} break;
	case CommonDeviceState::VirtualController:
	{
		const VirtualController *virt = this->castCheckedConst<VirtualController>();
		const VirtualControllerState *virtState =
			static_cast<const VirtualControllerState *>(sensor_state);

		// Only update the position filter when tracking is enabled
		post_imu_filter_packets_for_virtual_controller(
			virt, virtState,
			now, durationSinceLastUpdate,
			&m_PoseSensorIMUPacketQueue);
	} break;
    default:
        assert(0 && "Unhandled Controller Type");
    }

    // Consider this HMD state sequence num processed
    m_lastPollSeqNumProcessed = sensor_state->PollSequenceNumber;
}

void ServerControllerView::updateStateAndPredict()
{
	std::vector<PoseSensorPacket> timeSortedPackets;

	// Drain the packet queues filled by the threads
	PoseSensorPacket packet;
	while (m_PoseSensorIMUPacketQueue.try_dequeue(packet))
	{
		timeSortedPackets.push_back(packet);
	}

	//TODO: m_PoseSensorOpticalPacketQueue is currently getting filled on the main thread by
	// updateOpticalPoseEstimation() when triangulating the optical pose estimates.
	// Eventually this work will move to it's own camera processing thread 
	// this line will read from a lock-less queue just like the IMU packet queue.
	while (m_PoseSensorOpticalPacketQueue.size() > 0)
	{		
		timeSortedPackets.push_back(m_PoseSensorOpticalPacketQueue.front());
		m_PoseSensorOpticalPacketQueue.pop_front();
	}

	// Sort the packets in order of ascending time
	if (timeSortedPackets.size() > 1)
	{
		std::sort(
			timeSortedPackets.begin(), timeSortedPackets.end(), 
			[](const PoseSensorPacket & a, const PoseSensorPacket & b) -> bool
			{
				return a.timestamp < b.timestamp; 
			});

		t_high_resolution_duration duration= 
			timeSortedPackets[timeSortedPackets.size()-1].timestamp - timeSortedPackets[0].timestamp;
		std::chrono::duration<float, std::milli> milli_duration= duration;

		const size_t k_max_process_count= 100;
		if (timeSortedPackets.size() > k_max_process_count)
		{
			const size_t excess= timeSortedPackets.size() - k_max_process_count;

			SERVER_LOG_WARNING("updatePoseFilter()") << "Incoming packet count: " << timeSortedPackets.size() << " (" << milli_duration.count() << "ms)" << ", trimming: " << excess;
			timeSortedPackets.erase(timeSortedPackets.begin(), timeSortedPackets.begin()+excess);
		}
		else
		{
			//SERVER_LOG_DEBUG("updatePoseFilter()") << "Incoming packet count: " << timeSortedPackets.size() << " (" << milli_duration.count() << "ms)";
		}
	}

	// Process the sensor packets from oldest to newest
	for (const PoseSensorPacket &sensorPacket : timeSortedPackets)
    {
		// Compute the time since the last packet
		float time_delta_seconds;
		if (m_last_filter_update_timestamp_valid)
		{
			const std::chrono::duration<float, std::milli> time_delta = sensorPacket.timestamp - m_last_filter_update_timestamp;
			const float time_delta_milli = time_delta.count();

			// convert delta to seconds clamp time delta between 2500hz and 30hz
			time_delta_seconds = clampf(time_delta_milli / 1000.f, k_min_time_delta_seconds, k_max_time_delta_seconds);
		}
		else
		{
			time_delta_seconds = k_max_time_delta_seconds;
		}

		m_last_filter_update_timestamp = sensorPacket.timestamp;
		m_last_filter_update_timestamp_valid = true;

		{
			PoseFilterPacket filter_packet;
			filter_packet.clear();

			// Ship device id with the packet. We ned it for "OrientationExternal" filter.
			filter_packet.controllerDeviceId = this->getDeviceID();
			filter_packet.isSynced = TrackerManager::trackersSynced();

			// Create a filter input packet from the sensor data 
			// and the filter's previous orientation and position
			m_pose_filter_space->createFilterPacket(
				sensorPacket,
				m_pose_filter,
				filter_packet);

			// Process the filter packet
			m_pose_filter->update(time_delta_seconds, filter_packet);
		}
		
		// Flag the state as unpublished, which will trigger an update to the client
		markStateAsUnpublished();
	}
}

bool ServerControllerView::setHostBluetoothAddress(
    const std::string &address)
{
    return m_device->setHostBluetoothAddress(address);
}

CommonDevicePose
ServerControllerView::getFilteredPose(float time) const
{
    CommonDevicePose pose;

    pose.clear();

    if (m_pose_filter != nullptr)
    {
        const Eigen::Quaternionf orientation= m_pose_filter->getOrientation(time);
        const Eigen::Vector3f position_cm= m_pose_filter->getPositionCm(time);

        pose.Orientation.w= orientation.w();
        pose.Orientation.x= orientation.x();
        pose.Orientation.y= orientation.y();
        pose.Orientation.z= orientation.z();

        pose.PositionCm.x= position_cm.x();
        pose.PositionCm.y= position_cm.y();
        pose.PositionCm.z= position_cm.z();
    }

    return pose;
}

CommonDevicePhysics 
ServerControllerView::getFilteredPhysics() const
{
    CommonDevicePhysics physics;

    if (m_pose_filter != nullptr)
    {
        const Eigen::Vector3f first_derivative= m_pose_filter->getAngularVelocityRadPerSec();
        const Eigen::Vector3f second_derivative= m_pose_filter->getAngularAccelerationRadPerSecSqr();
        const Eigen::Vector3f velocity(m_pose_filter->getVelocityCmPerSec());
        const Eigen::Vector3f acceleration(m_pose_filter->getAccelerationCmPerSecSqr());

        physics.AngularVelocityRadPerSec.i = first_derivative.x();
        physics.AngularVelocityRadPerSec.j = first_derivative.y();
        physics.AngularVelocityRadPerSec.k = first_derivative.z();

        physics.AngularAccelerationRadPerSecSqr.i = second_derivative.x();
        physics.AngularAccelerationRadPerSecSqr.j = second_derivative.y();
        physics.AngularAccelerationRadPerSecSqr.k = second_derivative.z();

        physics.VelocityCmPerSec.i = velocity.x();
        physics.VelocityCmPerSec.j = velocity.y();
        physics.VelocityCmPerSec.k = velocity.z();

        physics.AccelerationCmPerSecSqr.i = acceleration.x();
        physics.AccelerationCmPerSecSqr.j = acceleration.y();
        physics.AccelerationCmPerSecSqr.k = acceleration.z();
    }

    return physics;
}

bool 
ServerControllerView::getIsBluetooth() const
{
    return (m_device != nullptr) ? m_device->getIsBluetooth() : false;
}

bool 
ServerControllerView::getUsesBluetoothAuthentication() const
{
	bool bUsesAuthentication= false;

	if (m_device != nullptr && m_device->getDeviceType() == CommonDeviceState::PSMove)
	{
        const PSMoveController *psmove = this->castCheckedConst<PSMoveController>();

		bUsesAuthentication= psmove->getIsPS4Controller();
	}

	return bUsesAuthentication;
}


bool
ServerControllerView::getIsStreamable() const
{
    bool bIsStreamableController= false;

    if (getIsOpen())
    {
        switch (getControllerDeviceType())
        {
        case CommonDeviceState::PSMove:
        case CommonDeviceState::PSDualShock4:
            {
                bIsStreamableController= getIsBluetooth();
            } break;
        case CommonDeviceState::PSNavi:
        case CommonDeviceState::VirtualController:
            {
                bIsStreamableController= true;
            } break;
        }
    }

    return bIsStreamableController;
}

bool 
ServerControllerView::getIsVirtualController() const
{
    return getIsOpen() && getControllerDeviceType() == CommonDeviceState::VirtualController;
}

// Returns the full usb device path for the controller
std::string 
ServerControllerView::getUSBDevicePath() const
{
    return (m_device != nullptr) ? m_device->getUSBDevicePath() : "";
}

// Returns the vendor ID of the controller
int 
ServerControllerView::getVendorID() const
{
    return (m_device != nullptr) ? m_device->getVendorID() : -1;
}

// Returns the product ID of the controller
int 
ServerControllerView::getProductID() const
{
    return (m_device != nullptr) ? m_device->getProductID() : -1;
}

// Returns the serial number for the controller
std::string 
ServerControllerView::getSerial() const
{
    return (m_device != nullptr) ? m_device->getSerial() : "";
}

// Returns the "controller_" + serial number for the controller
std::string
ServerControllerView::getConfigIdentifier() const
{
    std::string	identifier= "";

    if (m_device != nullptr)
    {
        std::string	prefix= "controller_";
        
        identifier= prefix+m_device->getSerial();
    }

    return identifier;
}

std::string 
ServerControllerView::getAssignedHostBluetoothAddress() const
{
    return (m_device != nullptr) ? m_device->getAssignedHostBluetoothAddress() : "";
}

CommonDeviceState::eDeviceType
ServerControllerView::getControllerDeviceType() const
{
    return m_device->getDeviceType();
}

const struct CommonControllerState * ServerControllerView::getState() const
{
    const struct CommonDeviceState *device_state = m_device->getState();
    assert(device_state == nullptr ||
        ((int)device_state->DeviceType >= (int)CommonDeviceState::Controller &&
        device_state->DeviceType < CommonDeviceState::SUPPORTED_CONTROLLER_TYPE_COUNT));

    return static_cast<const CommonControllerState *>(device_state);
}

bool ServerControllerView::getWasSystemButtonPressed() const
{
    bool bWasPressed= false;

    if (m_device != nullptr)
    {
        bWasPressed= m_device->getWasSystemButtonPressed();
    }

    return bWasPressed;
}

void ServerControllerView::setLEDOverride(unsigned char r, unsigned char g, unsigned char b)
{
    m_LED_override_color = std::make_tuple(r, g, b);
    m_LED_override_active = true;
    update_LED_color_internal();
}

void ServerControllerView::clearLEDOverride()
{
    m_LED_override_color = std::make_tuple(0x00, 0x00, 0x00);
    m_LED_override_active = false;
    update_LED_color_internal();
}

eCommonTrackingColorID ServerControllerView::getTrackingColorID() const
{
    eCommonTrackingColorID tracking_color_id = eCommonTrackingColorID::INVALID_COLOR;

    if (m_device != nullptr)
    {
        m_device->getTrackingColorID(tracking_color_id);
    }

    return tracking_color_id;
}

bool ServerControllerView::setTrackingColorID(eCommonTrackingColorID colorID)
{
    bool bSuccess= true;

    if (colorID != getTrackingColorID())
    {
        if (m_device != nullptr)
        {
            bSuccess= m_device->setTrackingColorID(colorID);

            if (bSuccess && getIsTrackingEnabled())
            {
                set_tracking_enabled_internal(false);
                set_tracking_enabled_internal(true);
            }
        }
        else
        {
            bSuccess= false;
        }
    }

    return bSuccess;
}

void ServerControllerView::startTracking()
{
    if (!m_tracking_enabled)
    {
        set_tracking_enabled_internal(true);
    }

    ++m_tracking_listener_count;
}

void ServerControllerView::stopTracking()
{
    assert(m_tracking_listener_count > 0);
    --m_tracking_listener_count;

    if (m_tracking_listener_count <= 0 && m_tracking_enabled)
    {
        set_tracking_enabled_internal(false);
    }
}

void ServerControllerView::set_tracking_enabled_internal(bool bEnabled)
{
    if (m_tracking_enabled != bEnabled)
    {
        if (bEnabled)
        {
            assert(m_device != nullptr);

            eCommonTrackingColorID tracking_color_id= eCommonTrackingColorID::INVALID_COLOR;
            m_device->getTrackingColorID(tracking_color_id);

            switch (tracking_color_id)
            {
			case eCommonTrackingColorID::Magenta:
                m_tracking_color= std::make_tuple(0xFF, 0x00, 0xFF);
                break;
            case eCommonTrackingColorID::Cyan:
                m_tracking_color = std::make_tuple(0x00, 0xFF, 0xFF);
                break;
            case eCommonTrackingColorID::Yellow:
                m_tracking_color = std::make_tuple(0xFF, 0xFF, 0x00);
                break;
            case eCommonTrackingColorID::Red:
                m_tracking_color = std::make_tuple(0xFF, 0x00, 0x00);
                break;
            case eCommonTrackingColorID::Green:
                m_tracking_color = std::make_tuple(0x00, 0xFF, 0x00);
                break;
			case eCommonTrackingColorID::Blue:
				m_tracking_color = std::make_tuple(0x00, 0x00, 0xFF);
				break;
            default:
				m_tracking_color = std::make_tuple(0x00, 0x00, 0x00);
				break;
            }
        }
        else
        {
            m_tracking_color = std::make_tuple(0x00, 0x00, 0x00);
        }

        m_tracking_enabled = bEnabled;

        update_LED_color_internal();
    }
}

void ServerControllerView::update_LED_color_internal()
{
    unsigned char r, g, b;
    if (m_LED_override_active)
    {
        r = std::get<0>(m_LED_override_color);
        g = std::get<1>(m_LED_override_color);
        b = std::get<2>(m_LED_override_color);
    }
    else if (m_tracking_enabled)
    {
        r = std::get<0>(m_tracking_color);
        g = std::get<1>(m_tracking_color);
        b = std::get<2>(m_tracking_color);
    }
    else
    {
        r = g = b = 0;
    }

    switch (getControllerDeviceType())
    {
    case CommonDeviceState::PSMove:
        {
            this->castChecked<PSMoveController>()->setLED(r, g, b);
        } break;
    case CommonDeviceState::PSNavi:
        {
            // Do nothing...
        } break;
    case CommonDeviceState::PSDualShock4:
        {
            this->castChecked<PSDualShock4Controller>()->setLED(r, g, b);
        } break;
    case CommonDeviceState::VirtualController:
        {
            // Do nothing...
        } break;
    default:
        assert(false && "Unhanded controller type!");
    }
}

// Get the tracking shape for the controller
bool ServerControllerView::getTrackingShape(CommonDeviceTrackingShape &trackingShape) const
{
    m_device->getTrackingShape(trackingShape);

    return trackingShape.shape_type != eCommonTrackingShapeType::INVALID_SHAPE;
}

float ServerControllerView::getROIPredictionTime() const
{
    static float k_max_roi_prediction_speec_cm = 30.f;
    static float k_max_roi_prediction_time = 0.1f;

    const Eigen::Vector3f velocityCmPerSec= getPoseFilter()->getVelocityCmPerSec();
    const float speedCmPerSec= velocityCmPerSec.norm();
    const float predictionTime = clampf01(speedCmPerSec / k_max_roi_prediction_speec_cm)*k_max_roi_prediction_time;

    return predictionTime;
}

// Set the rumble value between 0.f - 1.f on a given channel
bool ServerControllerView::setControllerRumble(
	float rumble_amount,
	CommonControllerState::RumbleChannel channel)
{
	bool result = false;

	if (getIsOpen())
	{
		switch (getControllerDeviceType())
		{
		case CommonDeviceState::PSMove:
		{
			unsigned char rumble_byte = static_cast<unsigned char>(clampf01(rumble_amount)*255.f);

			static_cast<PSMoveController *>(m_device)->setRumbleIntensity(rumble_byte);
			result = true;
		} break;

		case CommonDeviceState::PSNavi:
		{
			result = false; // No rumble on the navi
		} break;

		case CommonDeviceState::PSDualShock4:
		{
			unsigned char rumble_byte = static_cast<unsigned char>(clampf01(rumble_amount)*255.f);
			PSDualShock4Controller *controller = static_cast<PSDualShock4Controller *>(m_device);

			if (channel == CommonControllerState::RumbleChannel::ChannelLeft ||
				channel == CommonControllerState::RumbleChannel::ChannelAll)
			{
				controller->setLeftRumbleIntensity(rumble_byte);
			}

			if (channel == CommonControllerState::RumbleChannel::ChannelRight ||
				channel == CommonControllerState::RumbleChannel::ChannelAll)
			{
				controller->setRightRumbleIntensity(rumble_byte);
			}

			result = true;
		} break;

		case CommonDeviceState::VirtualController:
		{
			result = false; // No rumble on the virtual controller
		} break;

		default:
			assert(false && "Unhanded controller type!");
		}
	}

	return result;
}

// Gets if the controller optical tracking is enabled or not
bool ServerControllerView::getControllerOpticalTrackingEnabled()
{
	bool result = true;

	if (getIsOpen())
	{
		switch (getControllerDeviceType())
		{
			case CommonDeviceState::PSMove:
			{
				PSMoveController *controller = static_cast<PSMoveController *>(m_device);

				result = controller->getConfig()->enable_optical_tracking;
			} break;

			case CommonDeviceState::VirtualController:
			{
				VirtualController *controller = static_cast<VirtualController *>(m_device);

				result = controller->getConfig()->enable_optical_tracking;
			} break;
		}
	}

	return result;
}

void ServerControllerView::publish_device_data_frame()
{
    // Tell the server request handler we want to send out controller updates.
    // This will call generate_controller_data_frame_for_stream for each listening connection.
    ServerRequestHandler::get_instance()->publish_controller_data_frame(
        this, &ServerControllerView::generate_controller_data_frame_for_stream);
}

void ServerControllerView::generate_controller_data_frame_for_stream(
    const ServerControllerView *controller_view,
    const ControllerStreamInfo *stream_info,
    PSMoveProtocol::DeviceOutputDataFrame *data_frame)
{
    PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket *controller_data_frame= 
        data_frame->mutable_controller_data_packet();

    controller_data_frame->set_controller_id(controller_view->getDeviceID());
    controller_data_frame->set_sequence_num(controller_view->m_sequence_number);
    controller_data_frame->set_isconnected(controller_view->getDevice()->getIsOpen());
	
    switch (controller_view->getControllerDeviceType())
    {
    case CommonControllerState::PSMove:
        {            
            generate_psmove_data_frame_for_stream(controller_view, stream_info, data_frame);
        } break;
    case CommonControllerState::PSNavi:
        {
            generate_psnavi_data_frame_for_stream(controller_view, stream_info, data_frame);
        } break;
    case CommonControllerState::PSDualShock4:
        {
            generate_psdualshock4_data_frame_for_stream(controller_view, stream_info, data_frame);
        } break;
    case CommonControllerState::VirtualController:
        {
            generate_virtual_controller_data_frame_for_stream(controller_view, stream_info, data_frame);
        } break;
    default:
        assert(0 && "Unhandled controller type");
    }

    data_frame->set_device_category(PSMoveProtocol::DeviceOutputDataFrame::CONTROLLER);
}

static void generate_psmove_data_frame_for_stream(
    const ServerControllerView *controller_view,
    const ControllerStreamInfo *stream_info,
    PSMoveProtocol::DeviceOutputDataFrame *data_frame)
{
    const PSMoveController *psmove_controller= controller_view->castCheckedConst<PSMoveController>();
    const IPoseFilter *pose_filter= controller_view->getPoseFilter();
    const PSMoveControllerConfig *psmove_config= psmove_controller->getConfig();
    const CommonControllerState *controller_state= controller_view->getState();
    const CommonDevicePose controller_pose = controller_view->getFilteredPose(psmove_config->prediction_time);

    auto *controller_data_frame= data_frame->mutable_controller_data_packet();
    auto *psmove_data_frame = controller_data_frame->mutable_psmove_state();
   
    if (controller_state != nullptr)
    {        
        assert(controller_state->DeviceType == CommonDeviceState::PSMove);
        const PSMoveControllerInputState * psmove_state= static_cast<const PSMoveControllerInputState *>(controller_state);

        psmove_data_frame->set_validhardwarecalibration(psmove_config->is_valid);
        psmove_data_frame->set_iscurrentlytracking(controller_view->getIsCurrentlyTracking());
        psmove_data_frame->set_istrackingenabled(controller_view->getIsTrackingEnabled());
        psmove_data_frame->set_isorientationvalid(pose_filter->getIsOrientationStateValid());
        psmove_data_frame->set_ispositionvalid(pose_filter->getIsPositionStateValid());

        psmove_data_frame->mutable_orientation()->set_w(controller_pose.Orientation.w);
        psmove_data_frame->mutable_orientation()->set_x(controller_pose.Orientation.x);
        psmove_data_frame->mutable_orientation()->set_y(controller_pose.Orientation.y);
        psmove_data_frame->mutable_orientation()->set_z(controller_pose.Orientation.z);

        if (stream_info->include_position_data)
        {
            psmove_data_frame->mutable_position_cm()->set_x(controller_pose.PositionCm.x);
            psmove_data_frame->mutable_position_cm()->set_y(controller_pose.PositionCm.y);
            psmove_data_frame->mutable_position_cm()->set_z(controller_pose.PositionCm.z);
        }
        else
        {
            psmove_data_frame->mutable_position_cm()->set_x(0);
            psmove_data_frame->mutable_position_cm()->set_y(0);
            psmove_data_frame->mutable_position_cm()->set_z(0);
        }

        psmove_data_frame->set_trigger_value(psmove_state->TriggerValue);
        psmove_data_frame->set_battery_value(psmove_state->BatteryValue);

        unsigned int button_bitmask= 0;
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::TRIANGLE, psmove_state->Triangle);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CIRCLE, psmove_state->Circle);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CROSS, psmove_state->Cross);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::SQUARE, psmove_state->Square);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::SELECT, psmove_state->Select);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::START, psmove_state->Start);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::PS, psmove_state->PS);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::MOVE, psmove_state->Move);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::TRIGGER, psmove_state->Trigger);
        controller_data_frame->set_button_down_bitmask(button_bitmask);

        // If requested, get the raw sensor data for the controller
        if (stream_info->include_raw_sensor_data)
        {
            auto *raw_sensor_data= psmove_data_frame->mutable_raw_sensor_data();

            // One frame: [mx, my, mz] 
            raw_sensor_data->mutable_magnetometer()->set_i(psmove_state->RawMag[0]);
            raw_sensor_data->mutable_magnetometer()->set_j(psmove_state->RawMag[1]);
            raw_sensor_data->mutable_magnetometer()->set_k(psmove_state->RawMag[2]);

            // Two frames: [[ax0, ay0, az0], [ax1, ay1, az1]] 
            // Take the most recent frame: [ax1, ay1, az1]
            raw_sensor_data->mutable_accelerometer()->set_i(psmove_state->RawAccel[1][0]);
            raw_sensor_data->mutable_accelerometer()->set_j(psmove_state->RawAccel[1][1]);
            raw_sensor_data->mutable_accelerometer()->set_k(psmove_state->RawAccel[1][2]);

            // Two frames: [[wx0, wy0, wz0], [wx1, wy1, wz1]] 
            // Take the most recent frame: [wx1, wy1, wz1]
            raw_sensor_data->mutable_gyroscope()->set_i(psmove_state->RawGyro[1][0]);
            raw_sensor_data->mutable_gyroscope()->set_j(psmove_state->RawGyro[1][1]);
            raw_sensor_data->mutable_gyroscope()->set_k(psmove_state->RawGyro[1][2]);
        }

        // If requested, get the calibrated sensor data for the controller
        if (stream_info->include_calibrated_sensor_data)
        {
            auto *calibrated_sensor_data = psmove_data_frame->mutable_calibrated_sensor_data();

            // One frame: [mx, my, mz] 
            calibrated_sensor_data->mutable_magnetometer()->set_i(psmove_state->CalibratedMag[0]);
            calibrated_sensor_data->mutable_magnetometer()->set_j(psmove_state->CalibratedMag[1]);
            calibrated_sensor_data->mutable_magnetometer()->set_k(psmove_state->CalibratedMag[2]);

            // Two frames: [[ax0, ay0, az0], [ax1, ay1, az1]] 
            // Take the most recent frame: [ax1, ay1, az1]
            calibrated_sensor_data->mutable_accelerometer()->set_i(psmove_state->CalibratedAccel[1][0]);
            calibrated_sensor_data->mutable_accelerometer()->set_j(psmove_state->CalibratedAccel[1][1]);
            calibrated_sensor_data->mutable_accelerometer()->set_k(psmove_state->CalibratedAccel[1][2]);

            // Two frames: [[wx0, wy0, wz0], [wx1, wy1, wz1]] 
            // Take the most recent frame: [wx1, wy1, wz1]
            calibrated_sensor_data->mutable_gyroscope()->set_i(psmove_state->CalibratedGyro[1][0]);
            calibrated_sensor_data->mutable_gyroscope()->set_j(psmove_state->CalibratedGyro[1][1]);
            calibrated_sensor_data->mutable_gyroscope()->set_k(psmove_state->CalibratedGyro[1][2]);
        }

        // If requested, get the raw tracker data for the controller
        if (stream_info->include_raw_tracker_data)
        {
            auto *raw_tracker_data = psmove_data_frame->mutable_raw_tracker_data();
            int selectedTrackerId= stream_info->selected_tracker_index;
            unsigned int validTrackerBitmask= 0;

            for (int trackerId = 0; trackerId < TrackerManager::k_max_devices; ++trackerId)
            {
                const ControllerOpticalPoseEstimation *positionEstimate= 
                    controller_view->getTrackerPoseEstimate(trackerId);

                if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
                {
                    validTrackerBitmask&= (1 << trackerId);

                    if (trackerId == selectedTrackerId)
                    {
                        if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
                        {
                            const CommonDevicePosition &trackerRelativePosition = positionEstimate->position_cm;
                            const ServerTrackerViewPtr tracker_view = DeviceManager::getInstance()->getTrackerViewPtr(selectedTrackerId);

                            // Project the 3d camera position back onto the tracker screen
                            {
                                const CommonDeviceScreenLocation trackerScreenLocation =
                                    tracker_view->projectTrackerRelativePosition(&trackerRelativePosition);
                                PSMoveProtocol::Pixel *pixel = raw_tracker_data->mutable_screen_location();

                                pixel->set_x(trackerScreenLocation.x);
                                pixel->set_y(trackerScreenLocation.y);
                            }

                            // Add the tracker relative 3d position
                            {
                                PSMoveProtocol::Position *position_cm= raw_tracker_data->mutable_relative_position_cm();
                        
                                position_cm->set_x(trackerRelativePosition.x);
                                position_cm->set_y(trackerRelativePosition.y);
                                position_cm->set_z(trackerRelativePosition.z);
                            }

                            // Add the tracker relative projection shapes
                            {
                                const CommonDeviceTrackingProjection &trackerRelativeProjection = 
                                    positionEstimate->projection;

                                assert(trackerRelativeProjection.shape_type == eCommonTrackingProjectionType::ProjectionType_Ellipse);
                                PSMoveProtocol::Ellipse *ellipse= raw_tracker_data->mutable_projected_sphere();
                                
                                ellipse->mutable_center()->set_x(trackerRelativeProjection.shape.ellipse.center.x);
                                ellipse->mutable_center()->set_y(trackerRelativeProjection.shape.ellipse.center.y);
                                ellipse->set_half_x_extent(trackerRelativeProjection.shape.ellipse.half_x_extent);
                                ellipse->set_half_y_extent(trackerRelativeProjection.shape.ellipse.half_y_extent);
                                ellipse->set_angle(trackerRelativeProjection.shape.ellipse.angle);
                            }

                            raw_tracker_data->set_tracker_id(selectedTrackerId);
                        }
                    }
                }
            }
            raw_tracker_data->set_valid_tracker_bitmask(validTrackerBitmask);

            {
                const ControllerOpticalPoseEstimation *poseEstimate = controller_view->getMulticamPoseEstimate();

                if (poseEstimate->bCurrentlyTracking)
                {
                    PSMoveProtocol::Position *position_cm = raw_tracker_data->mutable_multicam_position_cm();
                    position_cm->set_x(poseEstimate->position_cm.x);
                    position_cm->set_y(poseEstimate->position_cm.y);
                    position_cm->set_z(poseEstimate->position_cm.z);
                }
            }
        }

        // if requested, get the physics data for the controller
        if (stream_info->include_physics_data)
        {
            const CommonDevicePhysics controller_physics = controller_view->getFilteredPhysics();
            auto *physics_data = psmove_data_frame->mutable_physics_data();

            physics_data->mutable_velocity_cm_per_sec()->set_i(controller_physics.VelocityCmPerSec.i);
            physics_data->mutable_velocity_cm_per_sec()->set_j(controller_physics.VelocityCmPerSec.j);
            physics_data->mutable_velocity_cm_per_sec()->set_k(controller_physics.VelocityCmPerSec.k);

            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_i(controller_physics.AccelerationCmPerSecSqr.i);
            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_j(controller_physics.AccelerationCmPerSecSqr.j);
            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_k(controller_physics.AccelerationCmPerSecSqr.k);

            physics_data->mutable_angular_velocity_rad_per_sec()->set_i(controller_physics.AngularVelocityRadPerSec.i);
            physics_data->mutable_angular_velocity_rad_per_sec()->set_j(controller_physics.AngularVelocityRadPerSec.j);
            physics_data->mutable_angular_velocity_rad_per_sec()->set_k(controller_physics.AngularVelocityRadPerSec.k);

            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_i(controller_physics.AngularAccelerationRadPerSecSqr.i);
            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_j(controller_physics.AngularAccelerationRadPerSecSqr.j);
            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_k(controller_physics.AngularAccelerationRadPerSecSqr.k);
        }
    }   

    controller_data_frame->set_controller_type(PSMoveProtocol::PSMOVE);
}

static void generate_psnavi_data_frame_for_stream(
    const ServerControllerView *controller_view,
    const ControllerStreamInfo *stream_info,
    PSMoveProtocol::DeviceOutputDataFrame *data_frame)
{
    PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket *controller_data_frame = data_frame->mutable_controller_data_packet();
    PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket_PSNaviState *psnavi_data_frame = controller_data_frame->mutable_psnavi_state();

    const CommonControllerState *controller_state= controller_view->getState();

    if (controller_state != nullptr)
    {
        assert(controller_state->DeviceType == CommonDeviceState::PSNavi);
        const PSNaviControllerInputState *psnavi_state= static_cast<const PSNaviControllerInputState *>(controller_state);

        psnavi_data_frame->set_trigger_value(psnavi_state->Trigger);
        psnavi_data_frame->set_stick_xaxis(psnavi_state->Stick_XAxis);
        psnavi_data_frame->set_stick_yaxis(psnavi_state->Stick_YAxis);

        unsigned int button_bitmask= 0;
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L1, psnavi_state->L1);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L2, psnavi_state->L2);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::TRIGGER, psnavi_state->L2);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L3, psnavi_state->L3);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CIRCLE, psnavi_state->Circle);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CROSS, psnavi_state->Cross);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::PS, psnavi_state->PS);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::UP, psnavi_state->DPad_Up);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::RIGHT, psnavi_state->DPad_Right);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::DOWN, psnavi_state->DPad_Down);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::LEFT, psnavi_state->DPad_Left);
        controller_data_frame->set_button_down_bitmask(button_bitmask);
    }

    controller_data_frame->set_controller_type(PSMoveProtocol::PSNAVI);
}

static void generate_psdualshock4_data_frame_for_stream(
    const ServerControllerView *controller_view,
    const ControllerStreamInfo *stream_info,
    PSMoveProtocol::DeviceOutputDataFrame *data_frame)
{
    const PSDualShock4Controller *ds4_controller = controller_view->castCheckedConst<PSDualShock4Controller>();
    const IPoseFilter *pose_filter= controller_view->getPoseFilter();
    const PSDualShock4ControllerConfig *psmove_config = ds4_controller->getConfig();
    const CommonControllerState *controller_state = controller_view->getState();
    const CommonDevicePose controller_pose = controller_view->getFilteredPose(psmove_config->prediction_time);

    auto *controller_data_frame = data_frame->mutable_controller_data_packet();
    auto *psds4_data_frame = controller_data_frame->mutable_psdualshock4_state();

    if (controller_state != nullptr)
    {
        assert(controller_state->DeviceType == CommonDeviceState::PSDualShock4);
        const DualShock4ControllerInputState * psds4_state = static_cast<const DualShock4ControllerInputState *>(controller_state);

        psds4_data_frame->set_validhardwarecalibration(psmove_config->is_valid);
        psds4_data_frame->set_iscurrentlytracking(controller_view->getIsCurrentlyTracking());
        psds4_data_frame->set_istrackingenabled(controller_view->getIsTrackingEnabled());
        psds4_data_frame->set_isorientationvalid(pose_filter->getIsOrientationStateValid());
        psds4_data_frame->set_ispositionvalid(pose_filter->getIsPositionStateValid());

        psds4_data_frame->mutable_orientation()->set_w(controller_pose.Orientation.w);
        psds4_data_frame->mutable_orientation()->set_x(controller_pose.Orientation.x);
        psds4_data_frame->mutable_orientation()->set_y(controller_pose.Orientation.y);
        psds4_data_frame->mutable_orientation()->set_z(controller_pose.Orientation.z);

        if (stream_info->include_position_data)
        {
            psds4_data_frame->mutable_position_cm()->set_x(controller_pose.PositionCm.x);
            psds4_data_frame->mutable_position_cm()->set_y(controller_pose.PositionCm.y);
            psds4_data_frame->mutable_position_cm()->set_z(controller_pose.PositionCm.z);
        }
        else
        {
            psds4_data_frame->mutable_position_cm()->set_x(0);
            psds4_data_frame->mutable_position_cm()->set_y(0);
            psds4_data_frame->mutable_position_cm()->set_z(0);
        }

        psds4_data_frame->set_left_thumbstick_x(psds4_state->LeftAnalogX);
        psds4_data_frame->set_left_thumbstick_y(psds4_state->LeftAnalogY);

        psds4_data_frame->set_right_thumbstick_x(psds4_state->RightAnalogX);
        psds4_data_frame->set_right_thumbstick_y(psds4_state->RightAnalogY);

        psds4_data_frame->set_left_trigger_value(psds4_state->LeftTrigger);
        psds4_data_frame->set_right_trigger_value(psds4_state->RightTrigger);

        unsigned int button_bitmask = 0;
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::UP, psds4_state->DPad_Up);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::DOWN, psds4_state->DPad_Down);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::LEFT, psds4_state->DPad_Left);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::RIGHT, psds4_state->DPad_Right);

        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L1, psds4_state->L1);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::R1, psds4_state->R1);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L2, psds4_state->L2);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::R2, psds4_state->R2);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::L3, psds4_state->L3);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::R3, psds4_state->R3);

        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::TRIANGLE, psds4_state->Triangle);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CIRCLE, psds4_state->Circle);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::CROSS, psds4_state->Cross);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::SQUARE, psds4_state->Square);

        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::SHARE, psds4_state->Share);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::OPTIONS, psds4_state->Options);

        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::PS, psds4_state->PS);
        SET_BUTTON_BIT(button_bitmask, PSMoveProtocol::DeviceOutputDataFrame_ControllerDataPacket::TRACKPAD, psds4_state->TrackPadButton);
        controller_data_frame->set_button_down_bitmask(button_bitmask);

        // If requested, get the raw sensor data for the controller
        if (stream_info->include_raw_sensor_data)
        {
            auto *raw_sensor_data = psds4_data_frame->mutable_raw_sensor_data();

            raw_sensor_data->mutable_accelerometer()->set_i(psds4_state->RawAccelerometer[0]);
            raw_sensor_data->mutable_accelerometer()->set_j(psds4_state->RawAccelerometer[1]);
            raw_sensor_data->mutable_accelerometer()->set_k(psds4_state->RawAccelerometer[2]);

            raw_sensor_data->mutable_gyroscope()->set_i(psds4_state->RawGyro[0]);
            raw_sensor_data->mutable_gyroscope()->set_j(psds4_state->RawGyro[1]);
            raw_sensor_data->mutable_gyroscope()->set_k(psds4_state->RawGyro[2]);
        }

        // If requested, get the calibrated sensor data for the controller
        if (stream_info->include_calibrated_sensor_data)
        {
            auto *calibrated_sensor_data = psds4_data_frame->mutable_calibrated_sensor_data();

            calibrated_sensor_data->mutable_accelerometer()->set_i(psds4_state->CalibratedAccelerometer.i);
            calibrated_sensor_data->mutable_accelerometer()->set_j(psds4_state->CalibratedAccelerometer.j);
            calibrated_sensor_data->mutable_accelerometer()->set_k(psds4_state->CalibratedAccelerometer.k);

            calibrated_sensor_data->mutable_gyroscope()->set_i(psds4_state->CalibratedGyro.i);
            calibrated_sensor_data->mutable_gyroscope()->set_j(psds4_state->CalibratedGyro.j);
            calibrated_sensor_data->mutable_gyroscope()->set_k(psds4_state->CalibratedGyro.k);
        }

        // If requested, get the raw tracker data for the controller
        if (stream_info->include_raw_tracker_data)
        {
            auto *raw_tracker_data = psds4_data_frame->mutable_raw_tracker_data();
            int selectedTrackerId= stream_info->selected_tracker_index;
            unsigned int validTrackerBitmask= 0;

            for (int trackerId = 0; trackerId < TrackerManager::k_max_devices; ++trackerId)
            {
                const ControllerOpticalPoseEstimation *positionEstimate= 
                    controller_view->getTrackerPoseEstimate(trackerId);

                if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
                {
                    validTrackerBitmask&= (1 << trackerId);

                    if (trackerId == selectedTrackerId)
                    {
                        const ControllerOpticalPoseEstimation *poseEstimate =
                            controller_view->getTrackerPoseEstimate(selectedTrackerId);

                        if (poseEstimate != nullptr && poseEstimate->bCurrentlyTracking)
                        {
                            const CommonDevicePosition &trackerRelativePosition = poseEstimate->position_cm;
                            const CommonDeviceQuaternion &trackerRelativeOrientation = poseEstimate->orientation;
                            const ServerTrackerViewPtr tracker_view = DeviceManager::getInstance()->getTrackerViewPtr(selectedTrackerId);
							
                            // Add the tracker relative 3d pose
                            {
                                PSMoveProtocol::Position *position = raw_tracker_data->mutable_relative_position_cm();
                                PSMoveProtocol::Orientation *orientation = raw_tracker_data->mutable_relative_orientation();

                                position->set_x(trackerRelativePosition.x);
                                position->set_y(trackerRelativePosition.y);
                                position->set_z(trackerRelativePosition.z);

                                orientation->set_w(trackerRelativeOrientation.w);
                                orientation->set_x(trackerRelativeOrientation.x);
                                orientation->set_y(trackerRelativeOrientation.y);
                                orientation->set_z(trackerRelativeOrientation.z);
                            }

                            // Add the tracker relative projection shapes
                            {
                                const CommonDeviceTrackingProjection &trackerRelativeProjection =
                                    poseEstimate->projection;

                                assert(trackerRelativeProjection.shape_type == eCommonTrackingProjectionType::ProjectionType_LightBar);
                                PSMoveProtocol::Polygon *polygon = raw_tracker_data->mutable_projected_blob();

                                for (int vert_index = 0; vert_index < 3; ++vert_index)
                                {
                                    PSMoveProtocol::Pixel *pixel = polygon->add_vertices();

                                    pixel->set_x(trackerRelativeProjection.shape.lightbar.triangle[vert_index].x);
                                    pixel->set_y(trackerRelativeProjection.shape.lightbar.triangle[vert_index].y);
                                }

                                CommonDeviceScreenLocation center_pixel;
                                center_pixel.clear();

                                for (int vert_index = 0; vert_index < 4; ++vert_index)
                                {
                                    const CommonDeviceScreenLocation &screenLocation= trackerRelativeProjection.shape.lightbar.quad[vert_index];
                                    PSMoveProtocol::Pixel *pixel = polygon->add_vertices();

                                    pixel->set_x(screenLocation.x);
                                    pixel->set_y(screenLocation.y);

                                    center_pixel.x += screenLocation.x;
                                    center_pixel.y += screenLocation.y;
                                }

                                center_pixel.x /= 4.f;
                                center_pixel.y /= 4.f;

                                {
                                    PSMoveProtocol::Pixel *pixel = raw_tracker_data->mutable_screen_location();

                                    pixel->set_x(center_pixel.x);
                                    pixel->set_y(center_pixel.y);
                                }
                            }

                            raw_tracker_data->set_tracker_id(selectedTrackerId);
                        }
                    }
                }                
            }
            raw_tracker_data->set_valid_tracker_bitmask(validTrackerBitmask);

            {
                const ControllerOpticalPoseEstimation *poseEstimate = controller_view->getMulticamPoseEstimate();

                if (poseEstimate->bCurrentlyTracking)
                {
                    PSMoveProtocol::Position *position = raw_tracker_data->mutable_multicam_position_cm();
                    position->set_x(poseEstimate->position_cm.x);
                    position->set_y(poseEstimate->position_cm.y);
                    position->set_z(poseEstimate->position_cm.z);

                    if (poseEstimate->bOrientationValid)
                    {
                        PSMoveProtocol::Orientation *orientation = raw_tracker_data->mutable_multicam_orientation();
                        orientation->set_w(poseEstimate->orientation.w);
                        orientation->set_x(poseEstimate->orientation.x);
                        orientation->set_y(poseEstimate->orientation.y);
                        orientation->set_z(poseEstimate->orientation.z);
                    }
                }
            }
        }

        // if requested, get the physics data for the controller
        if (stream_info->include_physics_data)
        {
            const CommonDevicePhysics controller_physics = controller_view->getFilteredPhysics();
            auto *physics_data = psds4_data_frame->mutable_physics_data();

            physics_data->mutable_velocity_cm_per_sec()->set_i(controller_physics.VelocityCmPerSec.i);
            physics_data->mutable_velocity_cm_per_sec()->set_j(controller_physics.VelocityCmPerSec.j);
            physics_data->mutable_velocity_cm_per_sec()->set_k(controller_physics.VelocityCmPerSec.k);

            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_i(controller_physics.AccelerationCmPerSecSqr.i);
            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_j(controller_physics.AccelerationCmPerSecSqr.j);
            physics_data->mutable_acceleration_cm_per_sec_sqr()->set_k(controller_physics.AccelerationCmPerSecSqr.k);

            physics_data->mutable_angular_velocity_rad_per_sec()->set_i(controller_physics.AngularVelocityRadPerSec.i);
            physics_data->mutable_angular_velocity_rad_per_sec()->set_j(controller_physics.AngularVelocityRadPerSec.j);
            physics_data->mutable_angular_velocity_rad_per_sec()->set_k(controller_physics.AngularVelocityRadPerSec.k);

            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_i(controller_physics.AngularAccelerationRadPerSecSqr.i);
            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_j(controller_physics.AngularAccelerationRadPerSecSqr.j);
            physics_data->mutable_angular_acceleration_rad_per_sec_sqr()->set_k(controller_physics.AngularAccelerationRadPerSecSqr.k);
        }
    }

    controller_data_frame->set_controller_type(PSMoveProtocol::PSDUALSHOCK4);
}

static void generate_virtual_controller_data_frame_for_stream(
    const ServerControllerView *controller_view, const ControllerStreamInfo *stream_info, PSMoveProtocol::DeviceOutputDataFrame *data_frame)
{
    const VirtualController *virtual_controller= controller_view->castCheckedConst<VirtualController>();
    const IPoseFilter *pose_filter= controller_view->getPoseFilter();
    const VirtualControllerConfig *controller_config= virtual_controller->getConfig();
    const CommonControllerState *controller_state= controller_view->getState();
    const CommonDevicePose controller_pose = controller_view->getFilteredPose(controller_config->prediction_time);

	// ###Externet $TODO Emulate PSmove so we can transmit orientation data without changing the protocol.
	//					 This is super hacky. Sadly the gamepad functionality falls away(?).
	//					 Very untested!! May could be ustable but so far it works fine.
	if (controller_config->psmove_emulation)
	{
		auto *controller_data_frame = data_frame->mutable_controller_data_packet();
		auto *psmove_data_frame = controller_data_frame->mutable_psmove_state();

		if (controller_state != nullptr)
		{
			assert(controller_state->DeviceType == CommonDeviceState::VirtualController);
			const VirtualControllerState * virtual_controller_state = static_cast<const VirtualControllerState *>(controller_state);

			psmove_data_frame->set_validhardwarecalibration(true);
			psmove_data_frame->set_iscurrentlytracking(controller_view->getIsCurrentlyTracking());
			psmove_data_frame->set_istrackingenabled(controller_view->getIsTrackingEnabled());
			psmove_data_frame->set_isorientationvalid(pose_filter->getIsOrientationStateValid());
			psmove_data_frame->set_ispositionvalid(pose_filter->getIsPositionStateValid());

			psmove_data_frame->mutable_orientation()->set_w(controller_pose.Orientation.w);
			psmove_data_frame->mutable_orientation()->set_x(controller_pose.Orientation.x);
			psmove_data_frame->mutable_orientation()->set_y(controller_pose.Orientation.y);
			psmove_data_frame->mutable_orientation()->set_z(controller_pose.Orientation.z);

			if (stream_info->include_position_data)
			{
				psmove_data_frame->mutable_position_cm()->set_x(controller_pose.PositionCm.x);
				psmove_data_frame->mutable_position_cm()->set_y(controller_pose.PositionCm.y);
				psmove_data_frame->mutable_position_cm()->set_z(controller_pose.PositionCm.z);
			}
			else
			{
				psmove_data_frame->mutable_position_cm()->set_x(0);
				psmove_data_frame->mutable_position_cm()->set_y(0);
				psmove_data_frame->mutable_position_cm()->set_z(0);
			}

			// ###Externet $TODO Re-route gamepad buttons to emulated PSmove controller?
			controller_data_frame->set_button_down_bitmask(0);

			// If requested, get the raw tracker data for the controller
			if (stream_info->include_raw_tracker_data)
			{
				auto *raw_tracker_data = psmove_data_frame->mutable_raw_tracker_data();
				int selectedTrackerId = stream_info->selected_tracker_index;
				unsigned int validTrackerBitmask = 0;

				for (int trackerId = 0; trackerId < TrackerManager::k_max_devices; ++trackerId)
				{
					const ControllerOpticalPoseEstimation *positionEstimate =
						controller_view->getTrackerPoseEstimate(trackerId);

					if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
					{
						validTrackerBitmask &= (1 << trackerId);

						if (trackerId == selectedTrackerId)
						{
							const CommonDevicePosition &trackerRelativePosition = positionEstimate->position_cm;
							const ServerTrackerViewPtr tracker_view = DeviceManager::getInstance()->getTrackerViewPtr(trackerId);

							// Project the 3d camera position back onto the tracker screen
							{
								const CommonDeviceScreenLocation trackerScreenLocation =
									tracker_view->projectTrackerRelativePosition(&trackerRelativePosition);
								PSMoveProtocol::Pixel *pixel = raw_tracker_data->mutable_screen_location();

								pixel->set_x(trackerScreenLocation.x);
								pixel->set_y(trackerScreenLocation.y);
							}

							// Add the tracker relative 3d position
							{
								PSMoveProtocol::Position *position_cm = raw_tracker_data->mutable_relative_position_cm();

								position_cm->set_x(trackerRelativePosition.x);
								position_cm->set_y(trackerRelativePosition.y);
								position_cm->set_z(trackerRelativePosition.z);
							}

							// Add the tracker relative projection shapes
							{
								const CommonDeviceTrackingProjection &trackerRelativeProjection =
									positionEstimate->projection;

								assert(trackerRelativeProjection.shape_type == eCommonTrackingProjectionType::ProjectionType_Ellipse);
								PSMoveProtocol::Ellipse *ellipse = raw_tracker_data->mutable_projected_sphere();

								ellipse->mutable_center()->set_x(trackerRelativeProjection.shape.ellipse.center.x);
								ellipse->mutable_center()->set_y(trackerRelativeProjection.shape.ellipse.center.y);
								ellipse->set_half_x_extent(trackerRelativeProjection.shape.ellipse.half_x_extent);
								ellipse->set_half_y_extent(trackerRelativeProjection.shape.ellipse.half_y_extent);
								ellipse->set_angle(trackerRelativeProjection.shape.ellipse.angle);
							}

							raw_tracker_data->set_tracker_id(selectedTrackerId);
						}
					}
				}
				raw_tracker_data->set_valid_tracker_bitmask(validTrackerBitmask);

				{
					const ControllerOpticalPoseEstimation *poseEstimate = controller_view->getMulticamPoseEstimate();

					if (poseEstimate->bCurrentlyTracking)
					{
						PSMoveProtocol::Position *position_cm = raw_tracker_data->mutable_multicam_position_cm();
						position_cm->set_x(poseEstimate->position_cm.x);
						position_cm->set_y(poseEstimate->position_cm.y);
						position_cm->set_z(poseEstimate->position_cm.z);
					}
				}
			}

			// if requested, get the physics data for the controller
			if (stream_info->include_physics_data)
			{
				const CommonDevicePhysics controller_physics = controller_view->getFilteredPhysics();
				auto *physics_data = psmove_data_frame->mutable_physics_data();

				physics_data->mutable_velocity_cm_per_sec()->set_i(controller_physics.VelocityCmPerSec.i);
				physics_data->mutable_velocity_cm_per_sec()->set_j(controller_physics.VelocityCmPerSec.j);
				physics_data->mutable_velocity_cm_per_sec()->set_k(controller_physics.VelocityCmPerSec.k);

				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_i(controller_physics.AccelerationCmPerSecSqr.i);
				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_j(controller_physics.AccelerationCmPerSecSqr.j);
				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_k(controller_physics.AccelerationCmPerSecSqr.k);
			}
		}

		controller_data_frame->set_controller_type(PSMoveProtocol::PSMOVE);
	}
	else
	{
		auto *controller_data_frame = data_frame->mutable_controller_data_packet();
		auto *virtual_controller_data_frame = controller_data_frame->mutable_virtualcontroller_state();

		if (controller_state != nullptr)
		{
			assert(controller_state->DeviceType == CommonDeviceState::VirtualController);
			const VirtualControllerState * virtual_controller_state = static_cast<const VirtualControllerState *>(controller_state);

			virtual_controller_data_frame->set_iscurrentlytracking(controller_view->getIsCurrentlyTracking());
			virtual_controller_data_frame->set_istrackingenabled(controller_view->getIsTrackingEnabled());
			virtual_controller_data_frame->set_ispositionvalid(pose_filter->getIsPositionStateValid());

			if (stream_info->include_position_data)
			{
				virtual_controller_data_frame->mutable_position_cm()->set_x(controller_pose.PositionCm.x);
				virtual_controller_data_frame->mutable_position_cm()->set_y(controller_pose.PositionCm.y);
				virtual_controller_data_frame->mutable_position_cm()->set_z(controller_pose.PositionCm.z);
			}
			else
			{
				virtual_controller_data_frame->mutable_position_cm()->set_x(0);
				virtual_controller_data_frame->mutable_position_cm()->set_y(0);
				virtual_controller_data_frame->mutable_position_cm()->set_z(0);
			}

			// Always send the gamepad data
			controller_data_frame->set_button_down_bitmask(controller_state->AllButtons);

			virtual_controller_data_frame->set_numbuttons(virtual_controller_state->numButtons);
			virtual_controller_data_frame->set_productid(virtual_controller_state->productID);
			virtual_controller_data_frame->set_vendorid(virtual_controller_state->vendorID);

			for (int axisIndex = 0; axisIndex < virtual_controller_state->numAxes; ++axisIndex)
			{
				virtual_controller_data_frame->add_axisstates(virtual_controller_state->axisStates[axisIndex]);
			}

			// If requested, get the raw tracker data for the controller
			if (stream_info->include_raw_tracker_data)
			{
				auto *raw_tracker_data = virtual_controller_data_frame->mutable_raw_tracker_data();
				int selectedTrackerId = stream_info->selected_tracker_index;
				unsigned int validTrackerBitmask = 0;

				for (int trackerId = 0; trackerId < TrackerManager::k_max_devices; ++trackerId)
				{
					const ControllerOpticalPoseEstimation *positionEstimate =
						controller_view->getTrackerPoseEstimate(trackerId);

					if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
					{
						validTrackerBitmask &= (1 << trackerId);

						if (trackerId == selectedTrackerId)
						{
							const CommonDevicePosition &trackerRelativePosition = positionEstimate->position_cm;
							const ServerTrackerViewPtr tracker_view = DeviceManager::getInstance()->getTrackerViewPtr(trackerId);

							// Project the 3d camera position back onto the tracker screen
							{
								const CommonDeviceScreenLocation trackerScreenLocation =
									tracker_view->projectTrackerRelativePosition(&trackerRelativePosition);
								PSMoveProtocol::Pixel *pixel = raw_tracker_data->mutable_screen_location();

								pixel->set_x(trackerScreenLocation.x);
								pixel->set_y(trackerScreenLocation.y);
							}

							// Add the tracker relative 3d position
							{
								PSMoveProtocol::Position *position_cm = raw_tracker_data->mutable_relative_position_cm();

								position_cm->set_x(trackerRelativePosition.x);
								position_cm->set_y(trackerRelativePosition.y);
								position_cm->set_z(trackerRelativePosition.z);
							}

							// Add the tracker relative projection shapes
							{
								const CommonDeviceTrackingProjection &trackerRelativeProjection =
									positionEstimate->projection;

								assert(trackerRelativeProjection.shape_type == eCommonTrackingProjectionType::ProjectionType_Ellipse);
								PSMoveProtocol::Ellipse *ellipse = raw_tracker_data->mutable_projected_sphere();

								ellipse->mutable_center()->set_x(trackerRelativeProjection.shape.ellipse.center.x);
								ellipse->mutable_center()->set_y(trackerRelativeProjection.shape.ellipse.center.y);
								ellipse->set_half_x_extent(trackerRelativeProjection.shape.ellipse.half_x_extent);
								ellipse->set_half_y_extent(trackerRelativeProjection.shape.ellipse.half_y_extent);
								ellipse->set_angle(trackerRelativeProjection.shape.ellipse.angle);
							}

							raw_tracker_data->set_tracker_id(selectedTrackerId);
						}
					}
				}
				raw_tracker_data->set_valid_tracker_bitmask(validTrackerBitmask);

				{
					const ControllerOpticalPoseEstimation *poseEstimate = controller_view->getMulticamPoseEstimate();

					if (poseEstimate->bCurrentlyTracking)
					{
						PSMoveProtocol::Position *position_cm = raw_tracker_data->mutable_multicam_position_cm();
						position_cm->set_x(poseEstimate->position_cm.x);
						position_cm->set_y(poseEstimate->position_cm.y);
						position_cm->set_z(poseEstimate->position_cm.z);
					}
				}
			}

			// if requested, get the physics data for the controller
			if (stream_info->include_physics_data)
			{
				const CommonDevicePhysics controller_physics = controller_view->getFilteredPhysics();
				auto *physics_data = virtual_controller_data_frame->mutable_physics_data();

				physics_data->mutable_velocity_cm_per_sec()->set_i(controller_physics.VelocityCmPerSec.i);
				physics_data->mutable_velocity_cm_per_sec()->set_j(controller_physics.VelocityCmPerSec.j);
				physics_data->mutable_velocity_cm_per_sec()->set_k(controller_physics.VelocityCmPerSec.k);

				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_i(controller_physics.AccelerationCmPerSecSqr.i);
				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_j(controller_physics.AccelerationCmPerSecSqr.j);
				physics_data->mutable_acceleration_cm_per_sec_sqr()->set_k(controller_physics.AccelerationCmPerSecSqr.k);
			}
		}

		controller_data_frame->set_controller_type(PSMoveProtocol::VIRTUALCONTROLLER);
	}

}

static IPoseFilter *
pose_filter_factory(
    const CommonDeviceState::eDeviceType deviceType,
    const std::string &position_filter_type,
    const std::string &orientation_filter_type,
    const PoseFilterConstants &constants)
{
    static IPoseFilter *filter= nullptr;

    if (position_filter_type == "PoseKalman" && orientation_filter_type == "PoseKalman")
    {
        switch (deviceType)
        {
        case CommonDeviceState::PSMove:
        case CommonDeviceState::VirtualController:
            {
                KalmanPoseFilterPSMove *kalmanFilter = new KalmanPoseFilterPSMove();
                kalmanFilter->init(constants);
                filter= kalmanFilter;
            } break;
        case CommonDeviceState::PSDualShock4:
            {
                KalmanPoseFilterDS4 *kalmanFilter = new KalmanPoseFilterDS4();
                kalmanFilter->init(constants);
                filter= kalmanFilter;
            } break;
        default:
            assert(0 && "unreachable");
        }
    }
    else
    {
        // Convert the position filter type string into an enum
        PositionFilterType position_filter_enum= PositionFilterTypeNone;
        if (position_filter_type == "")
        {
            position_filter_enum= PositionFilterTypeNone;
        }
        else if (position_filter_type == "PassThru")
        {
            position_filter_enum= PositionFilterTypePassThru;
        }
        else if (position_filter_type == "LowPassOptical")
        {
            position_filter_enum= PositionFilterTypeLowPassOptical;
        }
        else if (position_filter_type == "LowPassIMU")
        {
            position_filter_enum= PositionFilterTypeLowPassIMU;
        }
        else if (position_filter_type == "LowPassExponential")
        {
            position_filter_enum = PositionFilterTypeLowPassExponential;
        }
        else if (position_filter_type == "ComplimentaryOpticalIMU")
        {
            position_filter_enum= PositionFilterTypeComplimentaryOpticalIMU;
        }
		else if (position_filter_type == "PositionKalman")
		{
			position_filter_enum = PositionFilterTypeKalman;
		}
		else if (position_filter_type == "PositionExternalAttachment")
		{
			position_filter_enum = PositionFilterTypeExternalAttachment;
		}
        else
        {
            SERVER_LOG_INFO("pose_filter_factory()") << 
                "Unknown position filter type: " << position_filter_type << ". Using default.";

            // fallback to a default based on controller type
            switch (deviceType)
            {
            case CommonDeviceState::PSMove:
            case CommonDeviceState::VirtualController:
                position_filter_enum= PositionFilterTypeLowPassOptical;
                break;
            case CommonDeviceState::PSDualShock4:
                position_filter_enum= PositionFilterTypeComplimentaryOpticalIMU;
                break;
            default:
                assert(0 && "unreachable");
            }
        }
        
        // Convert the orientation filter type string into an enum
        OrientationFilterType orientation_filter_enum= OrientationFilterTypeNone;
        if (orientation_filter_type == "")
        {
            orientation_filter_enum= OrientationFilterTypeNone;
        }
        else if (orientation_filter_type == "PassThru")
        {
            orientation_filter_enum= OrientationFilterTypePassThru;
        }
        else if (orientation_filter_type == "MadgwickARG")
        {
            orientation_filter_enum= OrientationFilterTypeMadgwickARG;
        }
        else if (orientation_filter_type == "MadgwickMARG")
        {
            orientation_filter_enum= OrientationFilterTypeMadgwickMARG;
        }
        else if (orientation_filter_type == "ComplementaryOpticalARG")
        {
            orientation_filter_enum= OrientationFilterTypeComplementaryOpticalARG;
        }
        else if (orientation_filter_type == "ComplementaryMARG")
        {
            orientation_filter_enum= OrientationFilterTypeComplementaryMARG;
        }
		else if (orientation_filter_type == "OrientationKalman")
		{
			orientation_filter_enum = OrientationFilterTypeKalman;
		}
		else if (orientation_filter_type == "OrientationExternal")
		{
			orientation_filter_enum = OrientationFilterTypeExternal;
		}
        else
        {
            SERVER_LOG_INFO("pose_filter_factory()") << 
                "Unknown orientation filter type: " << orientation_filter_type << ". Using default.";

            // fallback to a default based on controller type
            switch (deviceType)
            {
            case CommonDeviceState::PSMove:
                orientation_filter_enum= OrientationFilterTypeComplementaryMARG;
                break;
            case CommonDeviceState::PSDualShock4:
                orientation_filter_enum= OrientationFilterTypeComplementaryOpticalARG;
                break;
            case CommonDeviceState::VirtualController:
                orientation_filter_enum= OrientationFilterTypeExternal;
                break;
            default:
                assert(0 && "unreachable");
            }
        }

        CompoundPoseFilter *compound_pose_filter = new CompoundPoseFilter();
        compound_pose_filter->init(deviceType, orientation_filter_enum, position_filter_enum, constants);
        filter= compound_pose_filter;
    }

    assert(filter != nullptr);

    return filter;
}

static void
init_filters_for_psmove(
    const PSMoveController *psmoveController, 
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter)
{
    const PSMoveControllerConfig *psmove_config = psmoveController->getConfig();

        // Setup the space the orientation filter operates in
    PoseFilterSpace *pose_filter_space = new PoseFilterSpace();
    pose_filter_space->setIdentityGravity(Eigen::Vector3f(0.f, 1.f, 0.f));
    pose_filter_space->setIdentityMagnetometer(
        Eigen::Vector3f(psmove_config->magnetometer_identity.i,
            psmove_config->magnetometer_identity.j,
                        psmove_config->magnetometer_identity.k));
    pose_filter_space->setCalibrationTransform(*k_eigen_identity_pose_laying_flat);
    pose_filter_space->setSensorTransform(*k_eigen_sensor_transform_opengl);

    // Copy the pose filter constants from the controller config
    PoseFilterConstants constants;
    constants.clear();

	psmoveController->getTrackingShape(constants.orientation_constants.tracking_shape);
    constants.orientation_constants.gravity_calibration_direction = pose_filter_space->getGravityCalibrationDirection();
    constants.orientation_constants.accelerometer_variance =
        Eigen::Vector3f(psmove_config->accelerometer_variance, psmove_config->accelerometer_variance, psmove_config->accelerometer_variance);
    constants.position_constants.accelerometer_drift = Eigen::Vector3f::Zero();
    constants.orientation_constants.magnetometer_calibration_direction = pose_filter_space->getMagnetometerCalibrationDirection();

    constants.orientation_constants.gyro_drift= 
        Eigen::Vector3f(psmove_config->gyro_drift, psmove_config->gyro_drift, psmove_config->gyro_drift);
    constants.orientation_constants.gyro_variance= 
        Eigen::Vector3f(psmove_config->gyro_variance, psmove_config->gyro_variance, psmove_config->gyro_variance);
    constants.orientation_constants.mean_update_time_delta= psmove_config->mean_update_time_delta;
    constants.orientation_constants.position_variance_curve.A = psmove_config->position_variance_exp_fit_a;
    constants.orientation_constants.position_variance_curve.B = psmove_config->position_variance_exp_fit_b;
    constants.orientation_constants.orientation_variance_curve.A = psmove_config->orientation_variance;
    constants.orientation_constants.orientation_variance_curve.B = 0.f;
    constants.orientation_constants.orientation_variance_curve.MaxValue = psmove_config->orientation_variance;
    constants.orientation_constants.magnetometer_variance= 
        Eigen::Vector3f(psmove_config->magnetometer_variance, psmove_config->magnetometer_variance, psmove_config->magnetometer_variance);
    constants.orientation_constants.magnetometer_drift = Eigen::Vector3f::Zero();

    constants.position_constants.gravity_calibration_direction = pose_filter_space->getGravityCalibrationDirection();
    constants.position_constants.accelerometer_variance= 
        Eigen::Vector3f(psmove_config->accelerometer_variance, psmove_config->accelerometer_variance, psmove_config->accelerometer_variance);
    constants.position_constants.accelerometer_drift = Eigen::Vector3f::Zero();
    constants.position_constants.accelerometer_noise_radius= psmove_config->accelerometer_noise_radius;
    constants.position_constants.max_velocity= psmove_config->max_velocity;
    constants.position_constants.mean_update_time_delta= psmove_config->mean_update_time_delta;
    constants.position_constants.position_variance_curve.A = psmove_config->position_variance_exp_fit_a;
    constants.position_constants.position_variance_curve.B = psmove_config->position_variance_exp_fit_b;
    constants.position_constants.position_variance_curve.MaxValue = 1.f;

    *out_pose_filter_space= pose_filter_space;
    *out_pose_filter= pose_filter_factory(
        CommonDeviceState::eDeviceType::PSMove,
        psmove_config->position_filter_type,
        psmove_config->orientation_filter_type,
        constants);
    }

static void
init_filters_for_psdualshock4(
    const PSDualShock4Controller *ds4Controller,
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter)
{
    const PSDualShock4ControllerConfig *ds4_config = ds4Controller->getConfig();

        // Setup the space the orientation filter operates in
    PoseFilterSpace *pose_filter_space = new PoseFilterSpace();
    pose_filter_space->setIdentityGravity(
            Eigen::Vector3f(
                ds4_config->identity_gravity_direction.i,
                ds4_config->identity_gravity_direction.j,
            ds4_config->identity_gravity_direction.k));
    pose_filter_space->setIdentityMagnetometer(Eigen::Vector3f::Zero());  // No magnetometer on DS4 :(
    pose_filter_space->setCalibrationTransform(*k_eigen_identity_pose_upright);
    pose_filter_space->setSensorTransform(*k_eigen_sensor_transform_identity);

    // Copy the pose filter constants from the controller config
    PoseFilterConstants constants;
    constants.clear();

	ds4Controller->getTrackingShape(constants.orientation_constants.tracking_shape);
    constants.orientation_constants.gravity_calibration_direction = pose_filter_space->getGravityCalibrationDirection();
    constants.orientation_constants.magnetometer_calibration_direction = pose_filter_space->getMagnetometerCalibrationDirection();
    constants.orientation_constants.mean_update_time_delta= ds4_config->mean_update_time_delta;
    constants.orientation_constants.magnetometer_drift = Eigen::Vector3f::Zero(); // no magnetometer on ds4
    constants.orientation_constants.magnetometer_variance= Eigen::Vector3f::Zero(); // no magnetometer on ds4
    constants.orientation_constants.accelerometer_drift = Eigen::Vector3f::Zero();
    constants.orientation_constants.accelerometer_variance =
        Eigen::Vector3f(ds4_config->accelerometer_variance, ds4_config->accelerometer_variance, ds4_config->accelerometer_variance);
    constants.orientation_constants.gyro_drift =
        Eigen::Vector3f(ds4_config->gyro_drift, ds4_config->gyro_drift, ds4_config->gyro_drift);
    constants.orientation_constants.gyro_variance=
        Eigen::Vector3f(ds4_config->gyro_variance, ds4_config->gyro_variance, ds4_config->gyro_variance);
    constants.orientation_constants.position_variance_curve.A = ds4_config->position_variance_exp_fit_a;
    constants.orientation_constants.position_variance_curve.B = ds4_config->position_variance_exp_fit_b;
    constants.orientation_constants.orientation_variance_curve.A = ds4_config->orientation_variance_exp_fit_a;
    constants.orientation_constants.orientation_variance_curve.B = ds4_config->orientation_variance_exp_fit_b;
    constants.orientation_constants.orientation_variance_curve.MaxValue = 1.f;

    constants.position_constants.use_linear_acceleration = ds4_config->position_use_linear_acceleration;
    constants.position_constants.apply_gravity_mask = ds4_config->position_apply_gravity_mask;
    constants.position_constants.gravity_calibration_direction= pose_filter_space->getGravityCalibrationDirection();
    constants.position_constants.accelerometer_drift = Eigen::Vector3f::Zero();
    constants.position_constants.accelerometer_variance= 
        Eigen::Vector3f(ds4_config->accelerometer_variance, ds4_config->accelerometer_variance, ds4_config->accelerometer_variance);
    constants.position_constants.accelerometer_noise_radius= ds4_config->accelerometer_noise_radius;
    constants.position_constants.max_velocity= ds4_config->max_velocity;
    constants.position_constants.mean_update_time_delta= ds4_config->mean_update_time_delta;
    constants.position_constants.position_variance_curve.A = ds4_config->position_variance_exp_fit_a;
    constants.position_constants.position_variance_curve.B = ds4_config->position_variance_exp_fit_b;
    constants.position_constants.position_variance_curve.MaxValue = 1.f;

    *out_pose_filter_space= pose_filter_space;
    *out_pose_filter= pose_filter_factory(
        CommonDeviceState::eDeviceType::PSDualShock4,
        ds4_config->position_filter_type,
        ds4_config->orientation_filter_type,
        constants);
}

static void init_filters_for_virtual_controller(
    const VirtualController *virtualController,
    PoseFilterSpace **out_pose_filter_space,
    IPoseFilter **out_pose_filter)
{
    const VirtualControllerConfig *controller_config = virtualController->getConfig();

	// Setup the space the pose filter operates in
	PoseFilterSpace *pose_filter_space = new PoseFilterSpace();
	pose_filter_space->setIdentityGravity(Eigen::Vector3f(0.f, 1.f, 0.f));
	pose_filter_space->setIdentityMagnetometer(Eigen::Vector3f::Zero());
	pose_filter_space->setCalibrationTransform(*k_eigen_identity_pose_upright);
	pose_filter_space->setSensorTransform(*k_eigen_sensor_transform_identity);

	// Copy the pose filter constants from the controller config
	PoseFilterConstants constants;
	constants.clear();

	virtualController->getTrackingShape(constants.orientation_constants.tracking_shape);
	constants.orientation_constants.gravity_calibration_direction = Eigen::Vector3f::Zero();
	constants.orientation_constants.accelerometer_variance = Eigen::Vector3f::Zero();
	constants.position_constants.accelerometer_drift = Eigen::Vector3f::Zero();
	constants.orientation_constants.magnetometer_calibration_direction = Eigen::Vector3f::Zero();
	constants.orientation_constants.gyro_drift = Eigen::Vector3f::Zero();
	constants.orientation_constants.gyro_variance = Eigen::Vector3f::Zero();
	constants.orientation_constants.mean_update_time_delta = 0.f;
	constants.orientation_constants.position_variance_curve.A = controller_config->position_variance_exp_fit_a;
	constants.orientation_constants.position_variance_curve.B = controller_config->position_variance_exp_fit_b;
	constants.orientation_constants.orientation_variance_curve.A = 0.f;
	constants.orientation_constants.orientation_variance_curve.B = 0.f;
	constants.orientation_constants.orientation_variance_curve.MaxValue = 0.f;
	constants.orientation_constants.magnetometer_variance = Eigen::Vector3f::Zero();
	constants.orientation_constants.magnetometer_drift = Eigen::Vector3f::Zero();

	constants.position_constants.gravity_calibration_direction = pose_filter_space->getGravityCalibrationDirection();
	constants.position_constants.accelerometer_variance = Eigen::Vector3f::Zero();
	constants.position_constants.accelerometer_drift = Eigen::Vector3f::Zero();
	constants.position_constants.accelerometer_noise_radius = 0.f; // TODO
	constants.position_constants.max_velocity = controller_config->max_velocity;
	constants.position_constants.mean_update_time_delta = controller_config->mean_update_time_delta;
	constants.position_constants.position_variance_curve.A = controller_config->position_variance_exp_fit_a;
	constants.position_constants.position_variance_curve.B = controller_config->position_variance_exp_fit_b;
	constants.position_constants.position_variance_curve.MaxValue = 1.f;

	*out_pose_filter_space = pose_filter_space;
	*out_pose_filter = pose_filter_factory(
		CommonDeviceState::eDeviceType::VirtualController,
		controller_config->position_filter_type,
		"OrientationExternal",
		constants);
}

static void post_imu_filter_packets_for_psmove(
	const PSMoveController *psmove, 
	const PSMoveControllerInputState *psmoveState,
	const t_high_resolution_timepoint now,
	const t_high_resolution_duration duration_since_last_update,
	t_controller_pose_sensor_queue *pose_filter_queue)
{
    const PSMoveControllerConfig *config = psmove->getConfig();

    PoseSensorPacket sensor_packet;

    sensor_packet.clear();

	// One magnetometer update for every two accel/gryo readings
	if (psmove->getSupportsMagnetometer())
	{
		sensor_packet.raw_imu_magnetometer = 
			{psmoveState->RawMag[0], psmoveState->RawMag[1], psmoveState->RawMag[2]};
        sensor_packet.imu_magnetometer_unit =
            Eigen::Vector3f(psmoveState->CalibratedMag[0], psmoveState->CalibratedMag[1], psmoveState->CalibratedMag[2]);
		sensor_packet.has_magnetometer_measurement= true;
	}

	if (psmove->getIsPS4Controller())
	{
		const int frame= 0;

		sensor_packet.timestamp= now;

		sensor_packet.raw_imu_accelerometer = {
			psmoveState->RawAccel[frame][0], 
			psmoveState->RawAccel[frame][1], 
			psmoveState->RawAccel[frame][2]};
		sensor_packet.imu_accelerometer_g_units =
			Eigen::Vector3f(
				psmoveState->CalibratedAccel[frame][0], 
				psmoveState->CalibratedAccel[frame][1], 
				psmoveState->CalibratedAccel[frame][2]);
		sensor_packet.has_accelerometer_measurement= true;

		sensor_packet.raw_imu_gyroscope = {
			psmoveState->RawGyro[frame][0], 
			psmoveState->RawGyro[frame][1], 
			psmoveState->RawGyro[frame][2]};
		sensor_packet.imu_gyroscope_rad_per_sec =
			Eigen::Vector3f(
				psmoveState->CalibratedGyro[frame][0], 
				psmoveState->CalibratedGyro[frame][1], 
				psmoveState->CalibratedGyro[frame][2]);
		sensor_packet.has_gyroscope_measurement= true;

		pose_filter_queue->enqueue(sensor_packet);
	}
	else
	{
		// Don't bother with the earlier frame if this is the very first IMU packet 
		// (since we have no previous timestamp to use)
		int start_frame_index= 0;
		if (duration_since_last_update == t_high_resolution_duration::zero())
		{
			start_frame_index= 1;
		}

		const t_high_resolution_timepoint prev_timestamp= now - (duration_since_last_update / 2);
		t_high_resolution_timepoint timestamps[2] = {prev_timestamp, now};

		// Each state update contains two readings (one earlier and one later) of accelerometer and gyro data
		for (int frame = start_frame_index; frame < 2; ++frame)
		{
			sensor_packet.timestamp= timestamps[frame];

			sensor_packet.raw_imu_accelerometer = {
				psmoveState->RawAccel[frame][0], 
				psmoveState->RawAccel[frame][1], 
				psmoveState->RawAccel[frame][2]};
			sensor_packet.imu_accelerometer_g_units =
				Eigen::Vector3f(
					psmoveState->CalibratedAccel[frame][0], 
					psmoveState->CalibratedAccel[frame][1], 
					psmoveState->CalibratedAccel[frame][2]);
			sensor_packet.has_accelerometer_measurement= true;

			sensor_packet.raw_imu_gyroscope = {
				psmoveState->RawGyro[frame][0], 
				psmoveState->RawGyro[frame][1], 
				psmoveState->RawGyro[frame][2]};
			sensor_packet.imu_gyroscope_rad_per_sec =
				Eigen::Vector3f(
					psmoveState->CalibratedGyro[frame][0], 
					psmoveState->CalibratedGyro[frame][1], 
					psmoveState->CalibratedGyro[frame][2]);
			sensor_packet.has_gyroscope_measurement= true;

			pose_filter_queue->enqueue(sensor_packet);
		}
	}
}

static void post_optical_filter_packet_for_psmove(
    const PSMoveController *psmove,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *pose_estimation,
	t_controller_pose_optical_queue *pose_filter_queue)
{
    const PSMoveControllerConfig *config = psmove->getConfig();
    PoseSensorPacket sensor_packet;

    sensor_packet.clear();
	sensor_packet.timestamp= now;

    // PSMove cant do optical orientation
    sensor_packet.optical_orientation = Eigen::Quaternionf::Identity();

	// PSMove does have an optical position
    if (pose_estimation->bCurrentlyTracking)
    {
		sensor_packet.optical_position_cm =
		Eigen::Vector3f(
				pose_estimation->position_cm.x,
				pose_estimation->position_cm.y,
				pose_estimation->position_cm.z);
		sensor_packet.tracking_projection_area_px_sqr= pose_estimation->projection.screen_area;
    }

	pose_filter_queue->push_back(sensor_packet);
}

static void post_imu_filter_packets_for_ds4(
    const PSDualShock4Controller *ds4, 
	const DualShock4ControllerInputState *ds4State,
    const t_high_resolution_timepoint now, 
	const t_high_resolution_duration duration_since_last_update,
	t_controller_pose_sensor_queue *pose_filter_queue)
{
    const PSDualShock4ControllerConfig *config = ds4->getConfig();

    PoseSensorPacket sensor_packet;

    sensor_packet.clear();

	sensor_packet.timestamp= now;

	sensor_packet.raw_imu_accelerometer = {
		ds4State->RawAccelerometer[0], 
		ds4State->RawAccelerometer[1], 
		ds4State->RawAccelerometer[2]};
    sensor_packet.imu_accelerometer_g_units =
        Eigen::Vector3f(
            ds4State->CalibratedAccelerometer.i, 
            ds4State->CalibratedAccelerometer.j, 
            ds4State->CalibratedAccelerometer.k);
	sensor_packet.has_accelerometer_measurement= true;

	sensor_packet.raw_imu_gyroscope = {
		ds4State->RawGyro[0], 
		ds4State->RawGyro[1], 
		ds4State->RawGyro[2]};
    sensor_packet.imu_gyroscope_rad_per_sec =
        Eigen::Vector3f(
            ds4State->CalibratedGyro.i, 
            ds4State->CalibratedGyro.j, 
            ds4State->CalibratedGyro.k);
	sensor_packet.has_gyroscope_measurement= true;

    pose_filter_queue->enqueue(sensor_packet);
}

// Send dummy IMU packets to force filters to work.
static void post_imu_filter_packets_for_virtual_controller(
	const VirtualController *psmove,
	const VirtualControllerState *psmoveState,
	const t_high_resolution_timepoint now,
	const t_high_resolution_duration duration_since_last_update,
	t_controller_pose_sensor_queue *pose_filter_queue)
{
	PoseSensorPacket sensor_packet;
	sensor_packet.clear();

	sensor_packet.timestamp = now;

	pose_filter_queue->enqueue(sensor_packet);
}

static void post_optical_filter_packet_for_ds4(
    const PSDualShock4Controller *ds4,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *pose_estimation,
	t_controller_pose_optical_queue *pose_filter_queue)
{
    const PSDualShock4ControllerConfig *config = ds4->getConfig();

    PoseSensorPacket sensor_packet;

    sensor_packet.clear();
	sensor_packet.timestamp= now;

    if (pose_estimation->bOrientationValid)
    {
        sensor_packet.optical_orientation = 
            Eigen::Quaternionf(
                pose_estimation->orientation.w, 
                pose_estimation->orientation.x,
                pose_estimation->orientation.y,
                pose_estimation->orientation.z);
    }

    if (pose_estimation->bCurrentlyTracking)
    {
		const float screen_area =
			(pose_estimation->projection.screen_area > config->min_screen_projection_area)
			? pose_estimation->projection.screen_area : 0.f;

		sensor_packet.optical_position_cm =
		Eigen::Vector3f(
				pose_estimation->position_cm.x,
				pose_estimation->position_cm.y,
				pose_estimation->position_cm.z);
		sensor_packet.tracking_projection_area_px_sqr= screen_area;
    }

	pose_filter_queue->push_back(sensor_packet);
}

static void post_optical_filter_packet_for_virtual_controller(
    const VirtualController *virtual_controller,
    const t_high_resolution_timepoint now,
    const ControllerOpticalPoseEstimation *pose_estimation,
	t_controller_pose_optical_queue *pose_filter_queue)
{
    const VirtualControllerConfig *config = virtual_controller->getConfig();

    PoseSensorPacket sensor_packet;

    sensor_packet.clear();
	sensor_packet.timestamp= now;

	// Virtual controllers don't currently support an optical orientation
	sensor_packet.optical_orientation = Eigen::Quaternionf::Identity();
	
    if (pose_estimation->bCurrentlyTracking)
    {
		sensor_packet.optical_position_cm =
		Eigen::Vector3f(
				pose_estimation->position_cm.x,
				pose_estimation->position_cm.y,
				pose_estimation->position_cm.z);
		sensor_packet.tracking_projection_area_px_sqr= pose_estimation->projection.screen_area;
    }

	pose_filter_queue->push_back(sensor_packet);
}

static void computeSpherePoseForControllerFromSingleTracker(
    const ServerControllerView *controllerView,
    const ServerTrackerViewPtr tracker,
    ControllerOpticalPoseEstimation *tracker_pose_estimation,
    ControllerOpticalPoseEstimation *multicam_pose_estimation)
{
    // No orientation for the sphere projection
    multicam_pose_estimation->orientation.clear();
    multicam_pose_estimation->bOrientationValid = false;

    // For the sphere projection, the tracker relative position has already been computed
    // Put the tracker relative position into world space
    multicam_pose_estimation->position_cm = tracker->computeWorldPosition(&tracker_pose_estimation->position_cm);
    multicam_pose_estimation->bCurrentlyTracking = true;

    // Copy over the screen projection area
    multicam_pose_estimation->projection.screen_area = tracker_pose_estimation->projection.screen_area;
}

static void computeLightBarPoseForControllerFromSingleTracker(
    const ServerControllerView *controllerView,
    const ServerTrackerViewPtr tracker,
    ControllerOpticalPoseEstimation *tracker_pose_estimation,
    ControllerOpticalPoseEstimation *multicam_pose_estimation)
{
    CommonDeviceTrackingShape tracking_shape;
    controllerView->getTrackingShape(tracking_shape);

    // Use the previous pose as a guess to the current pose
    const bool bPreviousPoseValid = tracker_pose_estimation->bOrientationValid;
    const CommonDevicePose poseGuess = { tracker_pose_estimation->position_cm, tracker_pose_estimation->orientation };

    // Compute a tracker relative position from the projection
    if (tracker->computePoseForProjection(
            &tracker_pose_estimation->projection,
            &tracking_shape,
            bPreviousPoseValid ? &poseGuess : nullptr,
            tracker_pose_estimation))
    {
        // If available, put the tracker relative orientation into world space
        if (tracker_pose_estimation->bOrientationValid)
        {
            multicam_pose_estimation->orientation = tracker->computeWorldOrientation(&tracker_pose_estimation->orientation);
            multicam_pose_estimation->bOrientationValid = true;
        }
        else
        {
            multicam_pose_estimation->orientation.clear();
            multicam_pose_estimation->bOrientationValid = false;
        }

        // Put the tracker relative position into world space
        multicam_pose_estimation->position_cm = tracker->computeWorldPosition(&tracker_pose_estimation->position_cm);
        multicam_pose_estimation->bCurrentlyTracking = true;
        
        // Copy over the screen projection area
        multicam_pose_estimation->projection.screen_area = tracker_pose_estimation->projection.screen_area;
    }
    else
    {
        multicam_pose_estimation->bCurrentlyTracking = false;
    }
}

static void computeSpherePoseForControllerFromMultipleTrackers(
    const ServerControllerView *controllerView,
    const TrackerManager* tracker_manager,
    const int *valid_projection_tracker_ids,
    const int projections_found,
    ControllerOpticalPoseEstimation *tracker_pose_estimations,
    ControllerOpticalPoseEstimation *multicam_pose_estimation)
{
	int available_trackers = 0;

	for (int tracker_id = 0; tracker_id < tracker_manager->getMaxDevices(); ++tracker_id)
	{
		ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);

		if (tracker->getIsOpen())
		{
			available_trackers++;
		}
	}
	
	const TrackerManagerConfig &cfg = tracker_manager->getConfig();
    float screen_area_sum = 0;
	
	struct projectionInfo
	{
		int index;
		int tracker_id;
		CommonDeviceScreenLocation position2d_list;
		float screen_area;
	};
	std::vector<projectionInfo> sorted_projections;

    // Project the tracker relative 3d tracking position back on to the tracker camera plane
    // and sum up the total controller projection area across all trackers
    for (int list_index = 0; list_index < projections_found; ++list_index)
    {
        const int tracker_id = valid_projection_tracker_ids[list_index];
        const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
        const ControllerOpticalPoseEstimation &poseEstimate = tracker_pose_estimations[tracker_id];

		projectionInfo info;
		info.index = list_index;
		info.tracker_id = tracker_id;
		info.position2d_list = tracker->projectTrackerRelativePosition(&poseEstimate.position_cm);
		info.screen_area = tracker_pose_estimations[tracker_id].projection.screen_area;
		sorted_projections.push_back(info);

		screen_area_sum += poseEstimate.projection.screen_area;
    }

	// Sort by biggest projector.
	// Go through all trackers and sort them by biggest projector to make tracking quality better.
	// The bigger projections should be closer to trackers and smaller far away.
	std::sort(
		sorted_projections.begin(), sorted_projections.end(),
		[](const projectionInfo & a, const projectionInfo & b) -> bool
	{
		return a.screen_area > b.screen_area;
	});

	// Compute triangulations amongst all pairs of projections
	int pair_count = 0;

    CommonDevicePosition average_world_position = { 0.f, 0.f, 0.f };
    for (int list_index = 0; list_index < projections_found; ++list_index)
    {
		int bad_deviations = 0;

		const int tracker_id = sorted_projections[list_index].tracker_id;
		const CommonDeviceScreenLocation &screen_location = sorted_projections[list_index].position2d_list;
		const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
		
		for (int other_list_index = 0; other_list_index < projections_found; ++other_list_index)
        {
			if (list_index == other_list_index)
				continue;

            const int other_tracker_id = sorted_projections[other_list_index].tracker_id;
            const CommonDeviceScreenLocation &other_screen_location = sorted_projections[other_list_index].position2d_list;
            const ServerTrackerViewPtr other_tracker = tracker_manager->getTrackerViewPtr(other_tracker_id);

            // if trackers are on poposite sides
            if (cfg.exclude_opposed_cameras)
            {
				//TODO: Use tracker FOV instead.
                if ((tracker->getTrackerPose().PositionCm.x > 0) == (other_tracker->getTrackerPose().PositionCm.x < 0) &&
                    (tracker->getTrackerPose().PositionCm.z > 0) == (other_tracker->getTrackerPose().PositionCm.z < 0))
                {
                    continue;
                }
            }

            // Using the screen locations on two different trackers we can triangulate a world position
            CommonDevicePosition world_position =
                ServerTrackerView::triangulateWorldPosition(
                    tracker.get(), &screen_location,
                    other_tracker.get(), &other_screen_location);

			// Check how much the trangulation deviates from other trackers.
			// Ignore its position if it deviates too much and renew its ROI.
			if (pair_count > 0 && cfg.max_tracker_position_deviation > 0.01f)
			{
				const float N = static_cast<float>(pair_count);

				if (abs((average_world_position.x / N) - world_position.x) < cfg.max_tracker_position_deviation
					&& abs((average_world_position.y / N) - world_position.y) < cfg.max_tracker_position_deviation
					&& abs((average_world_position.z / N) - world_position.z) < cfg.max_tracker_position_deviation)
				{ 
					average_world_position.x += world_position.x;
					average_world_position.y += world_position.y;
					average_world_position.z += world_position.z;

					++pair_count;
				}
				else
				{
					++bad_deviations;
				}
			}
			else
			{
				average_world_position.x += world_position.x;
				average_world_position.y += world_position.y;
				average_world_position.z += world_position.z;

				++pair_count;
			}
        }

		// What happend to that trackers projection? Its probably stuck somewhere on some color noise.
		// Enforce new ROI on this tracker to make it unstuck.
		if (bad_deviations >= projections_found - 1)
		{
			tracker_pose_estimations[tracker_id].bEnforceNewROI = true;
		}
    }

    if (pair_count == 0 
		&& sorted_projections.size() > 0 
		&& sorted_projections[0].tracker_id > -1 
		&& (available_trackers == 1 || !cfg.ignore_pose_from_one_tracker))
    {
        // Position not triangulated from opposed camera, estimate from one tracker only.
        computeSpherePoseForControllerFromSingleTracker(
            controllerView,
            tracker_manager->getTrackerViewPtr(sorted_projections[0].tracker_id),
            &tracker_pose_estimations[sorted_projections[0].tracker_id],
            multicam_pose_estimation);		
    }
    else if(pair_count > 0)
    {
        // Compute the average position
        const float N = static_cast<float>(pair_count);

        average_world_position.x /= N;
        average_world_position.y /= N;
        average_world_position.z /= N;

		// Do basic optical prediction
		const float pp = cfg.controller_position_prediction;
		if (pp > 0.01f)
		{
			const int controller_id = controllerView->getDeviceID();
			const int history_max_allowed = 50;

			const int ph = cfg.controller_position_prediction_history;

			int history_max = static_cast<int>(fmax(fmin(ph, history_max_allowed), 1.0f));

			static float average_world_position_history[PSMOVESERVICE_MAX_CONTROLLER_COUNT][history_max_allowed][3];
			static int history_count[PSMOVESERVICE_MAX_CONTROLLER_COUNT];

			average_world_position_history[controller_id][history_count[controller_id]][0] = average_world_position.x;
			average_world_position_history[controller_id][history_count[controller_id]][1] = average_world_position.y;
			average_world_position_history[controller_id][history_count[controller_id]][2] = average_world_position.z;

			history_count[controller_id] = ((history_count[controller_id] + 1) % history_max);

			CommonDevicePosition average_history = { 0.0f, 0.0f, 0.0f };

			for (int i = 0; i < history_max; i++)
			{
				average_history.x += average_world_position_history[controller_id][i][0];
				average_history.y += average_world_position_history[controller_id][i][1];
				average_history.z += average_world_position_history[controller_id][i][2];
			}

			average_history.x /= history_max;
			average_history.y /= history_max;
			average_history.z /= history_max;

			//const float dead_zone_distance = 2.0;

			//float last_distance = 2.0f;
			//last_distance = fmax(last_distance, abs(multicam_pose_estimation->position_cm.x - average_world_position.x));
			//last_distance = fmax(last_distance, abs(multicam_pose_estimation->position_cm.y - average_world_position.y));
			//last_distance = fmax(last_distance, abs(multicam_pose_estimation->position_cm.z - average_world_position.z));

			CommonDevicePosition average_scale;
			average_scale.x = (average_world_position.x - average_history.x) * pp; // lerp_clampf(0, pp, last_distance / dead_zone_distance);
			average_scale.y = (average_world_position.y - average_history.y) * pp; // lerp_clampf(0, pp, last_distance / dead_zone_distance);
			average_scale.z = (average_world_position.z - average_history.z) * pp; // lerp_clampf(0, pp, last_distance / dead_zone_distance);

			average_world_position.x += average_scale.x;
			average_world_position.y += average_scale.y;
			average_world_position.z += average_scale.z;
		}

		// Store the averaged tracking position
		const float q = fmin(cfg.controller_position_smoothing, 0.99f);
		if (q <= 0.01f)
		{
			multicam_pose_estimation->position_cm = average_world_position;
		}
		else
		{
			average_world_position.x = q * multicam_pose_estimation->position_cm.x + (1 - q) * average_world_position.x;
			average_world_position.y = q * multicam_pose_estimation->position_cm.y + (1 - q) * average_world_position.y;
			average_world_position.z = q * multicam_pose_estimation->position_cm.z + (1 - q) * average_world_position.z;
		}

		multicam_pose_estimation->position_cm = average_world_position;
        multicam_pose_estimation->bCurrentlyTracking = true;
    }

    // No orientation for the sphere projection
	multicam_pose_estimation->orientation.clear();
	multicam_pose_estimation->bOrientationValid = false;

    // Compute the average projection area.
    // This is proportional to our position tracking quality.
    multicam_pose_estimation->projection.screen_area =
        screen_area_sum / static_cast<float>(projections_found);
}

static void computeLightBarPoseForControllerFromMultipleTrackers(
    const ServerControllerView *controllerView,
    const TrackerManager* tracker_manager,
    const int *valid_projection_tracker_ids,
    const int projections_found,
    ControllerOpticalPoseEstimation *tracker_pose_estimations,
    ControllerOpticalPoseEstimation *multicam_pose_estimation)
{
    const int k_max_pairs = TrackerManager::k_max_devices*(TrackerManager::k_max_devices - 1);
    Eigen::Quaternionf world_orientations[k_max_pairs];
    float orientation_weights[k_max_pairs];
    float screen_area_sum = 0;

    // Compute triangulations amongst all pairs of projections
    int pair_count = 0;
    CommonDevicePosition average_world_position = { 0.f, 0.f, 0.f };
    for (int list_index = 0; list_index < projections_found; ++list_index)
    {
        const int tracker_id = valid_projection_tracker_ids[list_index];
        const CommonDeviceTrackingProjection &projection = tracker_pose_estimations[list_index].projection;
        const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);

        screen_area_sum += projection.screen_area;

        for (int other_list_index = list_index + 1; other_list_index < projections_found; ++other_list_index)
        {
            const int other_tracker_id = valid_projection_tracker_ids[other_list_index];
            const CommonDeviceTrackingProjection &other_projection = tracker_pose_estimations[other_list_index].projection;
            const ServerTrackerViewPtr other_tracker = tracker_manager->getTrackerViewPtr(other_tracker_id);

            // Using the screen locations on two different trackers we can triangulate a world position
            CommonDevicePose world_pose =
                ServerTrackerView::triangulateWorldPose(
                    tracker.get(), &projection,
                    other_tracker.get(), &other_projection);

            // Accumulate the position for averaging
            // TODO: Make this a weighted average
            average_world_position.x += world_pose.PositionCm.x;
            average_world_position.y += world_pose.PositionCm.y;
            average_world_position.z += world_pose.PositionCm.z;

            // Add the quaternion to a list for the purpose of averaging
            world_orientations[pair_count] = 
                Eigen::Quaternionf(
                    world_pose.Orientation.w,
                    world_pose.Orientation.x,
                    world_pose.Orientation.y, 
                    world_pose.Orientation.z);

            // Weight the quaternion base on how visible the controller is on each screen
            orientation_weights[pair_count] = projection.screen_area + other_projection.screen_area;

            ++pair_count;
        }
    }

    assert(pair_count >= 1);

    // Compute the average position
    {
        const float N = static_cast<float>(pair_count);

        average_world_position.x /= N;
        average_world_position.y /= N;
        average_world_position.z /= N;

        // Store the averaged tracking position
        multicam_pose_estimation->position_cm = average_world_position;
        multicam_pose_estimation->bCurrentlyTracking = true;
    }

    // Compute the average orientation
    {
        Eigen::Quaternionf avg_world_orientation;

        if (eigen_quaternion_compute_normalized_weighted_average(
                world_orientations,
                orientation_weights,
                pair_count,
                &avg_world_orientation))
        {
            multicam_pose_estimation->orientation.w = avg_world_orientation.w();
            multicam_pose_estimation->orientation.x = avg_world_orientation.x();
            multicam_pose_estimation->orientation.y = avg_world_orientation.y();
            multicam_pose_estimation->orientation.z = avg_world_orientation.z();
            multicam_pose_estimation->bOrientationValid = true;
        }
        else
        {
            multicam_pose_estimation->bOrientationValid = false;
        }
    }

    // Compute the average projection area.
    // This is proportional to our position tracking quality.
    multicam_pose_estimation->projection.screen_area =
        screen_area_sum / static_cast<float>(projections_found);
}
