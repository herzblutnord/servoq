#pragma once

#include "rust/cxx.h"

namespace servoq {

int run_qt_application(::rust::Vec<::rust::String> args);
::rust::String servo_profile_data_dir();
::rust::String system_cjk_font_family(::rust::Str generic);

}
