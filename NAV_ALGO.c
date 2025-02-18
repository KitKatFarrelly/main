#include <math.h>
#include <string.h>

#ifdef FUNCTIONAL_TESTS
#include "mocked_functions.h"
#else
#include "esp_log.h"
#endif

#include "NAV_ALGO.h"
#include "ToF_I2C.h"
#include "IMU_SPI.h"
#include "FLASH_SPI.h"

#define MAX_FEATURES_PER_TOF_ARRAY 10
#define MAX_GRADIENT_DIFF_FOR_FEATURE 50
#define MAX_GRADIENT_MAP_SIZE 8
#define DEGREES_PER_TOF_PIXEL 5.0
#define OFFSET_AT_MIDDLE_POSITION 2.5
#define RAD_TO_DEGREES 57.29578
#define DEGREES_TO_RAD 0.0174532925
#define DEGREES_TO_UINT8_T_ANGLE 0.7142857


typedef struct
{
    NAV_POINT_T robot_pos;
    int16_t submap_x;
    int16_t submap_z;
} robot_position_t;

typedef struct
{
    int16_t v_diff;
    int16_t h_diff;
    bool visited;
} gradient_graph_point_t;


typedef struct
{
    gradient_graph_point_t graph_points[MAX_GRADIENT_MAP_SIZE][MAX_GRADIENT_MAP_SIZE]; //use max size for gradient map
} gradient_map_t;

typedef struct
{
    dfs_feature_details_t node_details[MAX_FEATURES_PER_TOF_ARRAY];
    uint8_t number_of_features;
} feature_extraction_t;

static callback_handle_t s_nav_tof_handle;
static callback_handle_t s_nav_imu_handle;
static robot_position_t s_nav_robot_position;
static NAV_MAP_T s_nav_map;
static bool s_is_navigation_enabled = false;
static bool s_is_debug_enabled = false;

static gradient_map_t s_gradient_map = {0};

static const char *TAG = "NAV_ALG";

// Externs
component_handle_t nav_algo_public_component = 0;

static void nav_algo_queue_handler(component_handle_t component_type, uint8_t message_type, void* message_data, size_t message_size);
static uint8_t nav_algo_convert_adjusted_confidence_value(uint16_t distance, uint8_t confidence);
//placeholder for imu kalman filter function
static void nav_algo_check_tof_array_against_map(TOF_DATA_t* tof_data);

bool nav_algo_init(void)
{
    s_nav_tof_handle = register_priority_handler_for_messages(nav_algo_queue_handler, ToF_public_component);
    s_nav_imu_handle = register_priority_handler_for_messages(nav_algo_queue_handler, imu_public_component);
    if(check_is_queue_active(0))
	{
		create_handle_for_component(&nav_algo_public_component);
	}
    return true;
}

bool nav_algo_enable_navigation(bool enable)
{
    if(enable)
    {
        if(TOF_START_MEASUREMENTS())
        {
            return false;
        }
    }
    else
    {
        if(TOF_STOP_MEASUREMENTS())
        {
            return false;
        }
    }
    s_is_navigation_enabled = enable;
    return s_is_navigation_enabled;
}

bool nav_algo_restart_temp_map(void)
{
    return false;
}

NAV_MAP_HANDLER_T nav_algo_closest_map_to_temp_map(void)
{
    return NULL;
}

NAV_MAP_HANDLER_T nav_algo_start_writing_map(void)
{
    return NULL;
}

bool nav_algo_stop_writing_map(NAV_MAP_HANDLER_T map)
{
    return false;
}

bool nav_algo_save_map(NAV_MAP_HANDLER_T map_to_save)
{
    return false;
}

bool nav_algo_load_map(NAV_MAP_HANDLER_T map_to_load)
{
    return false;
}

NAV_MAP_HANDLER_T nav_algo_get_current_map(void)
{
    return NULL;
}

NAV_SUBMAP_T *nav_algo_get_submap(int16_t submap_x, int16_t submap_z)
{
    uint8_t submap_x_element = (uint8_t) ((submap_x + (MAX_POINTS_PER_SUBMAP / 2)) & 0xFF);
    uint8_t submap_z_element = (uint8_t) ((submap_z + (MAX_POINTS_PER_SUBMAP / 2)) & 0xFF);
    if(submap_x_element >= MAX_POINTS_PER_SUBMAP) submap_x_element = MAX_POINTS_PER_SUBMAP - 1;
    if(submap_z_element >= MAX_POINTS_PER_SUBMAP) submap_z_element = MAX_POINTS_PER_SUBMAP - 1;
    return &(s_nav_map.map[submap_x_element][submap_z_element]);
}

static void nav_algo_queue_handler(component_handle_t component_type, uint8_t message_type, void* message_data, size_t message_size)
{
    if(!s_is_navigation_enabled)
    {
        ESP_LOGI(TAG, "navigation not enabled, ignoring.");
        return;
    }
    if(component_type == ToF_public_component && message_type == TOF_MSG_NEW_DEPTH_ARRAY)
    {
        nav_algo_check_tof_array_against_map((TOF_DATA_t*) message_data);
    }
    else if(component_type == imu_public_component && message_type == IMU_MSG_RAW_DATA)
    {

    }
}

static uint8_t nav_algo_convert_adjusted_confidence_value(uint16_t distance, uint8_t confidence)
{
    float base_mult = 6.0;
    float square_val = (1000.0 / base_mult); //whatever the multiplier should be at 1000 mm
    float square_dist = (float) distance;
    float multiplier = 1.0 + ((square_dist * square_dist) / (square_val * square_val * base_mult));
    uint16_t ret_val = (uint16_t) (multiplier * (float) confidence);
    return (uint8_t) (ret_val & 0xFF);
}

static dfs_feature_details_t nav_algo_converge_details(dfs_feature_details_t first_det, dfs_feature_details_t second_det)
{
    dfs_feature_details_t return_details;
    return_details.number_of_nodes_in_feature = first_det.number_of_nodes_in_feature + second_det.number_of_nodes_in_feature;
    return_details.min_x = (first_det.min_x < second_det.min_x) ? first_det.min_x : second_det.min_x;
    return_details.max_x = (first_det.max_x < second_det.max_x) ? first_det.max_x : second_det.max_x;
    return_details.min_y = (first_det.min_y < second_det.min_y) ? first_det.min_y : second_det.min_y;
    return_details.max_y = (first_det.max_y < second_det.max_y) ? first_det.max_y : second_det.max_y;
    return_details.average_angle = (first_det.average_angle * first_det.number_of_nodes_in_feature) + (second_det.average_angle * second_det.number_of_nodes_in_feature);
    return_details.average_angle = return_details.average_angle / return_details.number_of_nodes_in_feature;
    return_details.average_distance = (first_det.average_distance * first_det.number_of_nodes_in_feature) + (second_det.average_distance * second_det.number_of_nodes_in_feature);
    return_details.average_distance = return_details.average_distance / return_details.number_of_nodes_in_feature;
    return_details.average_confidence = (first_det.average_confidence * first_det.number_of_nodes_in_feature) + (second_det.average_confidence * second_det.number_of_nodes_in_feature);
    return_details.average_confidence = return_details.average_confidence / return_details.number_of_nodes_in_feature;
    return return_details;
}

//create a new feature via dfs
static dfs_feature_details_t nav_algo_create_new_feature_with_dfs(uint8_t v_iter, uint8_t h_iter, TOF_DATA_t* tof_data)
{
    //dfs in each possible direction, then collect data and return to main
    double current_diff = (double) (h_iter < tof_data->horizontal_size - 1) ? s_gradient_map.graph_points[v_iter][h_iter].h_diff : s_gradient_map.graph_points[v_iter][h_iter - 1].h_diff;
    double current_pixel_diff = (((double) (h_iter) - 4.0) * DEGREES_PER_TOF_PIXEL) + OFFSET_AT_MIDDLE_POSITION;
    //need to calculate z distance per cell using sin, then calculate slope.
    double current_run = ((double) tof_data->depth_pixel_field[v_iter][h_iter]) * sin(DEGREES_TO_RAD * current_pixel_diff);
    //from there, use arctan to calculate angle.
    double angle = DEGREES_TO_UINT8_T_ANGLE * RAD_TO_DEGREES * atan2(current_diff, current_run);
    dfs_feature_details_t node_details = 
    {
        .number_of_nodes_in_feature = 1,
        .min_x = h_iter,
        .max_x = h_iter,
        .min_y = v_iter,
        .max_y = v_iter,
        .average_angle = ((int16_t) angle & 0x00FF),
        .average_distance = (tof_data->depth_pixel_field[v_iter][h_iter] & 0x0000FFFF),
        .average_confidence = ((tof_data->depth_pixel_field[v_iter][h_iter] >> 24) & 0x00FF),
    };
    s_gradient_map.graph_points[v_iter][h_iter].visited = true;
    //for vertical angle, we only want to add if the angle is flat.
    if(v_iter > 0 && !s_gradient_map.graph_points[v_iter - 1][h_iter].visited)
    {
        if(s_gradient_map.graph_points[v_iter - 1][h_iter].v_diff < MAX_GRADIENT_DIFF_FOR_FEATURE)
        {
            dfs_feature_details_t up_details = nav_algo_create_new_feature_with_dfs(v_iter - 1, h_iter, tof_data);
            node_details = nav_algo_converge_details(node_details, up_details);
        }
    }
    if(v_iter < tof_data->horizontal_size - 1 && !s_gradient_map.graph_points[v_iter + 1][h_iter].visited)
    {
        if(s_gradient_map.graph_points[v_iter][h_iter].v_diff < MAX_GRADIENT_DIFF_FOR_FEATURE)
        {
            dfs_feature_details_t down_details = nav_algo_create_new_feature_with_dfs(v_iter + 1, h_iter, tof_data);
            node_details = nav_algo_converge_details(node_details, down_details);
        }
    }
    //horizontal angles get calculated to determine angle of feature.
    if(h_iter > 0 && !s_gradient_map.graph_points[v_iter][h_iter - 1].visited)
    {
        int16_t left_diff = s_gradient_map.graph_points[v_iter][h_iter].h_diff - s_gradient_map.graph_points[v_iter][h_iter - 1].h_diff;
        if(left_diff < 0) left_diff = -left_diff; //invert if negative
        if(left_diff < MAX_GRADIENT_DIFF_FOR_FEATURE || s_gradient_map.graph_points[v_iter][h_iter - 1].h_diff < MAX_GRADIENT_DIFF_FOR_FEATURE)
        {
            dfs_feature_details_t left_details = nav_algo_create_new_feature_with_dfs(v_iter, h_iter - 1, tof_data);
            node_details = nav_algo_converge_details(node_details, left_details);
        }
    }
    if(h_iter < tof_data->horizontal_size - 1 && !s_gradient_map.graph_points[v_iter][h_iter + 1].visited)
    {
        int16_t right_diff = s_gradient_map.graph_points[v_iter][h_iter].h_diff - s_gradient_map.graph_points[v_iter][h_iter + 1].h_diff;
        if(right_diff < 0) right_diff = -right_diff; //invert if negative
        if(right_diff < MAX_GRADIENT_DIFF_FOR_FEATURE || s_gradient_map.graph_points[v_iter][h_iter].h_diff < MAX_GRADIENT_DIFF_FOR_FEATURE)
        {
            dfs_feature_details_t right_details = nav_algo_create_new_feature_with_dfs(v_iter, h_iter + 1, tof_data);
            node_details = nav_algo_converge_details(node_details, right_details);
        }
    }
    return node_details;
}

//perform feature extraction from tof data
//generate up to MAX_FEATURES_PER_TOF_ARRAY landmarks
static feature_extraction_t nav_algo_feature_extraction_from_tof_data(TOF_DATA_t* tof_data)
{
    feature_extraction_t return_features_list = {0};
    return_features_list.number_of_features = 0;
    //as all features are considered to be planes in this design, features are extracted like so:
    //1. create a 2x2 convolution of each point to determine vertical and horizontal gradient, starting from top left
    for(uint8_t v_iter = 0; v_iter < tof_data->horizontal_size; v_iter++)
    {
        for(uint8_t h_iter = 0; h_iter < tof_data->horizontal_size; h_iter++)
        {
            //negative values are just very high numbers for unsigned integers, which is fine in this case
            if(v_iter < tof_data->horizontal_size - 1)
            {
                s_gradient_map.graph_points[v_iter][h_iter].v_diff = (tof_data->depth_pixel_field[v_iter][h_iter] & 0xFFFF) - (tof_data->depth_pixel_field[v_iter + 1][h_iter] & 0xFFFF);
            }
            if(h_iter < tof_data->horizontal_size - 1)
            {
                s_gradient_map.graph_points[v_iter][h_iter].h_diff = (tof_data->depth_pixel_field[v_iter][h_iter] & 0xFFFF) - (tof_data->depth_pixel_field[v_iter][h_iter + 1] & 0xFFFF);
            }
            s_gradient_map.graph_points[v_iter][h_iter].visited = false;
        }
    }
    //2. dfs to find islands of features within the convolution with similar gradients.
    for(uint8_t v_iter = 0; v_iter < tof_data->horizontal_size - 1; v_iter++)
    {
        for(uint8_t h_iter = 0; h_iter < tof_data->horizontal_size - 1; h_iter++)
        {
            
            if(!s_gradient_map.graph_points[v_iter][h_iter].visited)
            {
                //create new feature via dfs
                dfs_feature_details_t new_node = nav_algo_create_new_feature_with_dfs(v_iter, h_iter, tof_data);
                if(return_features_list.number_of_features < MAX_FEATURES_PER_TOF_ARRAY)
                {
                    //add new node 
                    return_features_list.node_details[return_features_list.number_of_features] = new_node;
                    return_features_list.number_of_features++;
                }
                else
                {
                    //if new feature is larger than the smallest current feature then replace feature on list
                    uint8_t min_feature = 0;
                    //this can be done more efficiently...
                    for(uint8_t list_iter = 0; list_iter < return_features_list.number_of_features; list_iter++)
                    {
                        if(return_features_list.node_details[list_iter].number_of_nodes_in_feature < return_features_list.node_details[min_feature].number_of_nodes_in_feature ||
                        ((return_features_list.node_details[list_iter].number_of_nodes_in_feature == return_features_list.node_details[min_feature].number_of_nodes_in_feature) && 
                        (return_features_list.node_details[list_iter].average_distance < return_features_list.node_details[min_feature].average_distance)))
                        {
                            min_feature = list_iter;
                        }
                    }
                    if(new_node.number_of_nodes_in_feature > return_features_list.node_details[min_feature].number_of_nodes_in_feature)
                    {
                        return_features_list.node_details[min_feature].number_of_nodes_in_feature = new_node.number_of_nodes_in_feature;
                    }
                }
                
            }
        }
    }
    //3. Combine islands that are likely the same island, then order islands from largest to smallest in terms of number of cells.
    //4. determine center of mass of each island, xyz dimensions, as well as their orientation vs the robot.
    return return_features_list;
}

static NAV_POINT_T nav_algo_convert_node_details_to_landmark(dfs_feature_details_t details)
{
    NAV_POINT_T return_point;
    return_point.rotation = details.average_angle;
    return_point.confidence = details.average_confidence;
    double pixel_width = (double) (details.max_x - details.min_x + 1) * DEGREES_PER_TOF_PIXEL;
    double pixel_height = (double) (details.max_y - details.min_y + 1) * DEGREES_PER_TOF_PIXEL;
    double pixel_width_average = ((((double) (details.max_x + details.min_x)) / 2.0) - 3.5) * DEGREES_PER_TOF_PIXEL;
    double pixel_height_average = ((((double) (details.max_y + details.min_y)) / 2.0) - 3.5) * DEGREES_PER_TOF_PIXEL;
    if(pixel_width_average < 0) pixel_width_average = -pixel_width_average;
    if(pixel_height_average < 0) pixel_height_average = -pixel_height_average;
    double width = (double) details.average_distance * sin(pixel_width * DEGREES_TO_RAD);
    double height = (double) details.average_distance * sin(pixel_height * DEGREES_TO_RAD);
    //Need to fix this - minimum width/height should be 1. If height > 0x00FF, should be 0.
    return_point.width = ((uint32_t) width) * 0x00FF;
    return_point.height = ((uint32_t) height) * 0x00FF;
    //need to make sure these are all positive
    uint16_t z_dist = (uint16_t) (details.average_distance * cos(pixel_width_average * DEGREES_TO_RAD));
    uint16_t x_dist = (uint16_t) (details.average_distance * sin(pixel_width_average * DEGREES_TO_RAD));
    uint16_t y_dist = (uint16_t) (details.average_distance * sin(pixel_height_average * DEGREES_TO_RAD));
    return_point.xyz_pos = ((x_dist & 0x03FF) << 20) + ((y_dist & 0x03FF) << 10) + (z_dist & 0x03FF);
    return return_point;
}

static uint8_t nav_algo_move_robot_according_to_transform(NAV_POINT_T *current, NAV_POINT_T transform, uint8_t op)
{
    //y axis is up/down!!! need to make sure that this doesn't drift problematically for ground robots
    uint32_t current_z = current->xyz_pos & 0x000003FF;
    uint32_t current_y = current->xyz_pos & 0x000FFC00;
    uint32_t current_x = current->xyz_pos & 0x3FF00000;
    uint32_t transform_z = transform.xyz_pos & 0x000003FF;
    uint32_t transform_y = transform.xyz_pos & 0x000FFC00;
    uint32_t transform_x = transform.xyz_pos & 0x3FF00000;
    uint8_t return_overflow = 0;
    uint32_t new_z = 0;
    uint32_t new_y = 0;
    uint32_t new_x = 0;
    switch(op)
    {
        case 0: //add
            new_z = current_z + transform_z;
            new_y = current_y + transform_y;
            new_x = current_x + transform_x;
            current->rotation += transform.rotation;
            break;
        case 1: //subtract current - transform
            new_z = current_z - transform_z;
            new_y = current_y - transform_y;
            new_x = current_x - transform_x;
            current->rotation -= transform.rotation;
            break;
        case 2: //subtract transform - current
            new_z = transform_z - current_z;
            new_y = transform_y - current_y;
            new_x = transform_x - current_x;
            current->rotation = transform.rotation - current->rotation;
            break;
        case 3: //add, excepting the y vector
            new_z = transform_z - current_z;
            new_y = current_y;
            new_x = transform_x - current_x;
            current->rotation += transform.rotation;
            break;
        default:
            break;
    }
    if(new_z > 0x0400)
    {
        current->xyz_pos = new_z & 0x03FF;
        return_overflow = 1;
    }
    else if(new_z > 0x0800)
    {
        current->xyz_pos = (~new_z + 1) & 0x03FF;
        return_overflow = 2;
    }
    else
    {
        current->xyz_pos = new_z & 0x03FF;
    }
    if(new_y > 0x00100000)
    {
        current->xyz_pos += new_y & 0x000FFC00;
        return_overflow += 4;
    }
    else if(new_y > 0x00200000)
    {
        current->xyz_pos += (~new_y + 1) & 0x000FFC00;
        return_overflow += 8;
    }
    else
    {
        current->xyz_pos += new_y & 0x000FFC00;
    }
    if(new_x > 0x40000000)
    {
        current->xyz_pos += new_x & 0x3FF00000;
        return_overflow += 16;
    }
    else if(new_x > 0x80000000)
    {
        current->xyz_pos += (~new_x + 1) & 0x3FF00000;
        return_overflow += 32;
    }
    else
    {
        current->xyz_pos += new_x & 0x3FF00000;
    }
    return return_overflow;
}

//read tof map and run error vs existing map to estimate movement
//also create and adjust objects on each submap
static void nav_algo_check_tof_array_against_map(TOF_DATA_t* tof_data)
{
    //step 1: generate landmarks
    feature_extraction_t features_list = nav_algo_feature_extraction_from_tof_data(tof_data);

    if(!features_list.number_of_features)
    {
        //no features were extracted from the array.
        return;
    }

    NAV_POINT_T landmark_list[MAX_FEATURES_PER_TOF_ARRAY] = {0};
    for(uint8_t list_iter = 0; list_iter < features_list.number_of_features; list_iter++)
    {
        landmark_list[list_iter] = nav_algo_convert_node_details_to_landmark(features_list.node_details[list_iter]);
    }

    if(s_is_debug_enabled && check_is_queue_active(0))
    {
        //send features list to the message queue
		message_info_t convert_feature_msg;
        NAV_POINT_T* msg_features_list = malloc(features_list.number_of_features * sizeof(NAV_POINT_T));
        memcpy(msg_features_list, landmark_list, features_list.number_of_features * sizeof(NAV_POINT_T));
		convert_feature_msg.message_data = (void*) msg_features_list;
		convert_feature_msg.message_size = features_list.number_of_features * sizeof(NAV_POINT_T);
		convert_feature_msg.is_pointer = true;
		convert_feature_msg.component_handle = nav_algo_public_component;
		convert_feature_msg.message_type = NAV_RAW_FEATURE_DATA;
		send_message_to_normal_queue(convert_feature_msg);
    }

    //step 2: find 2 most confident landmarks
    //Sort landmark_list here?

    //step 3: determine rotation + translation error of landmark 1 to each existing landmark on submap
    NAV_POINT_T landmark_list_transformed[MAX_POINTS_PER_SUBMAP] = {0};
    NAV_POINT_T landmark_error[MAX_POINTS_PER_SUBMAP] = {0};
    
    NAV_SUBMAP_T * submap_pointer = nav_algo_get_submap(s_nav_robot_position.submap_x, s_nav_robot_position.submap_z); //maybe have a getter/setter for this...
    uint8_t closest_point = MAX_POINTS_PER_SUBMAP;
    uint32_t least_error = 0xFF000000;
    for(uint8_t iter = 0; iter < MAX_POINTS_PER_SUBMAP; iter++)
    {
        if(!submap_pointer->pointCloud[iter].confidence)
        {
            //break out once pointclouds start becoming invalid
            break;
        }
        //need to transform into the fram of reference of the robot position first
        landmark_list_transformed[iter].xyz_pos = landmark_list[0].xyz_pos;
        landmark_list_transformed[iter].rotation = landmark_list[0].rotation;
        nav_algo_move_robot_according_to_transform(&landmark_list_transformed[iter], s_nav_robot_position.robot_pos, 0);
        //calculate error of pointCloud[iter] - features_list[0]
        landmark_error[iter] = landmark_list_transformed[iter];
        nav_algo_move_robot_according_to_transform(&landmark_error[iter], submap_pointer->pointCloud[iter], 2);
        landmark_error[iter].width = submap_pointer->pointCloud[iter].width - landmark_list[0].width;
        landmark_error[iter].height = submap_pointer->pointCloud[iter].height - landmark_list[0].height;
        //if error is sufficiently close to s_nav_robot_position for any given feature in the pointcloud, we have matched the feature.
        //otherwise, continue with step 3.1

        //error cost function.
        uint32_t feature_error = (landmark_error[iter].rotation << 16) + (landmark_error[iter].width << 8) + landmark_error[iter].height;
        if(feature_error < least_error)
        {
            least_error = feature_error;
            closest_point = iter;
        }
    }

    if(s_is_debug_enabled && check_is_queue_active(0))
    {
        //send transform list to the message queue
		message_info_t transform_msg;
        NAV_POINT_T* msg_features_list = malloc(MAX_POINTS_PER_SUBMAP * sizeof(NAV_POINT_T));
        memcpy(msg_features_list, landmark_list, MAX_POINTS_PER_SUBMAP * sizeof(NAV_POINT_T));
		transform_msg.message_data = (void*) msg_features_list;
		transform_msg.message_size = MAX_POINTS_PER_SUBMAP * sizeof(NAV_POINT_T);
		transform_msg.is_pointer = true;
		transform_msg.component_handle = nav_algo_public_component;
		transform_msg.message_type = NAV_RAW_FEATURE_DATA;
		send_message_to_normal_queue(transform_msg);
    }



    //step 3.1: if necessary, determine rotation + translation error of landmark 2 to each existing landmark on submap

    //step 3.2: if necessary, compare each set of errors to build array of potential transforms based on lowest shared error.

    //step 3.3: if necessary, compare each potential transform against 3, 4, 5, etc landmarks until one landmark remains

    //step 4: update location

    //TODO: calculate which entry in landmark error list has the lowest error
    if(closest_point < MAX_POINTS_PER_SUBMAP)
    {
        uint8_t overflows = nav_algo_move_robot_according_to_transform(&(s_nav_robot_position.robot_pos), landmark_error[closest_point], 3);
        if(overflows & 0x01)
        {
            s_nav_robot_position.submap_z++;
        }
        if(overflows & 0x02)
        {
            s_nav_robot_position.submap_z--;
        }
        if(overflows & 0x10)
        {
            s_nav_robot_position.submap_x++;
        }
        if(overflows & 0x20)
        {
            s_nav_robot_position.submap_x--;
        }
    }
    
    //step 5: update submap with map landmark info

    //TODO: again, entry in landmark error list with lowest cost needs to be calculated
    //For testing purposes, just replace all the old features with the latest batch
    for(uint8_t iter_landmark = 0; iter_landmark < MAX_POINTS_PER_SUBMAP; iter_landmark++)
    {
        submap_pointer->pointCloud[iter_landmark] = landmark_list_transformed[iter_landmark];
    }
}

//Basic mapping idea:
//turn any given array into a set of landmarks and translate/rotate array according to estimated robot location
//calculate distances of one landmark in array to all landmarks in submap and calculate rotational error for each
//do the same for a second landmark in array.
//try to find minimum error between two rotational and translation vectors. Use average of the two vectors as array transform onto map
//update landmarks on map according to new landmarks from array

//enable sending debug messages to the message queue
bool nav_algo_enable_debug_messages(bool enable)
{
    return (s_is_debug_enabled = enable);
}