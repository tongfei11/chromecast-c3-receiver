#pragma once

#if __has_include("ReceiverConfig.local.h")
#include "ReceiverConfig.local.h"
#endif

#ifndef RECEIVER_TARGET_MAC
#define RECEIVER_TARGET_MAC ""
#endif

#ifndef RECEIVER_DEVICE_NAME
#define RECEIVER_DEVICE_NAME "Chromecast C3 Receiver"
#endif

#ifndef RECEIVER_SERIAL_BAUD
#define RECEIVER_SERIAL_BAUD 115200
#endif
