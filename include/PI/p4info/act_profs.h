/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PI_INC_PI_P4INFO_ACT_PROFS_H_
#define PI_INC_PI_P4INFO_ACT_PROFS_H_

#include "PI/pi_base.h"

pi_p4_id_t pi_p4info_act_prof_id_from_name(const pi_p4info_t *p4info,
                                           const char *name);

const char *pi_p4info_act_prof_name_from_id(const pi_p4info_t *p4info,
                                            pi_p4_id_t act_prof_id);

bool pi_p4info_act_prof_has_selector(const pi_p4info_t *p4info,
                                     pi_p4_id_t act_prof_id);

pi_p4_id_t pi_p4info_act_prof_get_table(const pi_p4info_t *p4info,
                                        pi_p4_id_t act_prof_id);

pi_p4_id_t pi_p4info_act_prof_begin(const pi_p4info_t *p4info);
pi_p4_id_t pi_p4info_act_prof_next(const pi_p4info_t *p4info, pi_p4_id_t id);
pi_p4_id_t pi_p4info_act_prof_end(const pi_p4info_t *p4info);

#endif  // PI_INC_PI_P4INFO_ACT_PROFS_H_
