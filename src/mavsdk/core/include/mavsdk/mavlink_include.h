#pragma once

#include "mavlink/mavlink_types.h"
extern mavlink_status_t m_mavlink_status[MAVLINK_COMM_NUM_BUFFERS];
extern mavlink_message_t m_mavlink_buffer[MAVLINK_COMM_NUM_BUFFERS];
#include "mavlink_include_build.h"
