# SPDX-FileCopyrightText: 2021 Comcast Cable Communications Management, LLC
# SPDX-License-Identifier: MIT

add_custom_target(license
                  COMMAND reuse lint
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
