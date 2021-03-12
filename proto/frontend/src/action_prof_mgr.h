/* Copyright 2013-present Barefoot Networks, Inc.
 * Copyright 2021 VMware, Inc.
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

/*
 * Antonin Bas
 *
 */

#ifndef SRC_ACTION_PROF_MGR_H_
#define SRC_ACTION_PROF_MGR_H_

#include <PI/frontends/cpp/tables.h>
#include <PI/pi.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>  // std::move
#include <vector>

#include "google/rpc/code.pb.h"
#include "google/rpc/status.pb.h"
#include "p4/v1/p4runtime.pb.h"

#include "bimap.h"
#include "common.h"
#include "report_error.h"
#include "statusor.h"

namespace pi {

namespace fe {

namespace proto {

using Status = ::google::rpc::Status;

class ActionProfBiMap {
 public:
  using Id = uint32_t;  // may change in the future

  void add(const Id &id, pi_indirect_handle_t h);

  // returns nullptr if no matching id
  const pi_indirect_handle_t *retrieve_handle(const Id &id) const;

  // returns nullptr if no matching handle
  const Id *retrieve_id(pi_indirect_handle_t h) const;

  void remove(const Id &id);

  bool empty() const;

 private:
  BiMap<Id, pi_indirect_handle_t> bimap;
};

using ActionProfMemberId = ActionProfBiMap::Id;
using ActionProfGroupId = ActionProfBiMap::Id;

// Support for weighted members assume that the underlying PI implementation has
// no native support for weights.
// - for each member_id, we keep track of the maximum weight for that member
//   (across all groups), this is done with the MemberState::weight_counts map.
// - if the maximum weight is W, maintain W copies of the member (created by
//   calling pi_act_prof_mbr_create W times with the same parameters) with W
//   different handles. Those handles are stored in the MemberState::handles
//   vector.
// - if a group includes the member with weight w (where w is <= W), then pick
//   w unique handles among the set of W handles we have and set the group
//   membership using all w handles.
// - delete members that are no longer needed (when W decreases).

class ActionProfMemberMap {
 public:
  using Id = ActionProfBiMap::Id;

  struct MemberState {
    // NOLINTNEXTLINE(whitespace/operators)
    explicit MemberState(pi::ActionData &&action_data)
        : action_data(std::move(action_data)) { }

    pi::ActionData action_data;
    std::vector<pi_indirect_handle_t> handles;
    std::map<int, int> weight_counts;
  };

  // NOLINTNEXTLINE(whitespace/operators)
  bool add(const Id &id, pi_indirect_handle_t h, pi::ActionData &&action_data);

  // returns nullptr if no matching id
  MemberState *access_member_state(const Id &id);

  // returns nullptr if no matching handle
  const Id *retrieve_id(pi_indirect_handle_t h) const;

  const pi_indirect_handle_t *get_first_handle(const Id &id) const;

  bool remove(const Id &id);

  bool add_handle(pi_indirect_handle_t h, const Id &id);

  bool remove_handle(pi_indirect_handle_t h);

  bool empty() const;

 private:
  std::unordered_map<Id, MemberState> members;
  std::unordered_map<pi_indirect_handle_t, Id> handle_to_id;
};

struct WatchPort {
  enum class WatchKindCase {
    kNotSet,
    kWatch,
    kWatchPort,
  };

  WatchKindCase watch_kind_case;
  int watch;
  std::string watch_port;
  pi_port_t pi_port;

  friend bool operator==(const WatchPort &lhs, const WatchPort &rhs);
  friend bool operator!=(const WatchPort &lhs, const WatchPort &rhs);

  static const WatchPort invalid_watch();

  static WatchPort make(const p4::v1::ActionProfileGroup::Member &member);
  static WatchPort make(const p4::v1::ActionProfileAction &action);

  void to_p4rt(p4::v1::ActionProfileGroup::Member *member) const;
  void to_p4rt(p4::v1::ActionProfileAction *action) const;

 private:
  template <typename T> void to_p4rt_helper(T *msg) const;
};

class ActionProfGroupMembership {
 public:
  using Id = ActionProfBiMap::Id;

  struct MembershipInfo {
    int weight;
    WatchPort watch;

    friend bool operator==(const MembershipInfo &lhs,
                           const MembershipInfo &rhs);
  };

  // Represents an update (insertion, deletion, change of weight) to group. A
  // list of such updates is generated by compute_membership_update. If the
  // member is unchanged (same weight), current_weight == new_weight.
  struct MembershipUpdate {
    MembershipUpdate(Id id, int current_weight, int new_weight,
                     const WatchPort &current_watch,
                     const WatchPort &new_watch)
        : id(id), current_weight(current_weight), new_weight(new_weight),
          current_watch(current_watch), new_watch(new_watch) { }

    Id id;
    int current_weight;
    int new_weight;
    WatchPort current_watch;
    WatchPort new_watch;
  };

  explicit ActionProfGroupMembership(size_t max_size_user);

  // desired membership must be sorted, with no duplicates (same member id)
  std::vector<MembershipUpdate> compute_membership_update(
      const std::map<Id, MembershipInfo> &desired_membership) const;

  size_t get_max_size_user() const;

  void set_membership(std::map<Id, MembershipInfo> &&new_members);

  const std::map<Id, MembershipInfo> &get_membership() const;

  std::map<Id, MembershipInfo> &get_membership();

  bool get_member_info(
      const Id &member_id, int *weight, WatchPort *watch) const;

 private:
  std::map<Id, MembershipInfo> members{};

  size_t max_size_user{0};
};

class WatchPortEnforcer;

class ActionProfAccessBase {
 public:
  using SessionTemp = common::SessionTemp;

  // The ActionProfMgr is essentially a frontend to the pi_act_prof_* methods in
  // the PI C library. PI offers 2 ways of programming action profile groups:
  // either by performing individual add & remove operations, or a more
  // intent-based way where the entire group membership is set with a single API
  // call. ActionProfMgr can integrate with PI using either one of these
  // programming methods.
  // Concretely when DeviceMgr instantiates new ActionProfMgr objects, it checks
  // which API is supported by the PI target implementation and use that one. If
  // both are supported, the intent-based API (SET_MEMBERSHIP) will be
  // preferred. This is done through the static ActionProfMgr::choose_pi_api
  // method.
  enum class PiApiChoice { INDIVIDUAL_ADDS_AND_REMOVES, SET_MEMBERSHIP };

  enum class SelectorUsage { UNSPECIFIED, ONESHOT, MANUAL };

  // ideally this should be protected, but the base classes are inheriting this
  // constructor
  ActionProfAccessBase(pi_dev_tgt_t device_tgt, pi_p4_id_t act_prof_id,
                       pi_p4info_t *p4info, PiApiChoice pi_api_choice,
                       WatchPortEnforcer *watch_port_enforcer);

  virtual ~ActionProfAccessBase() = default;

  virtual bool empty() const = 0;

 protected:
  bool check_p4_action_id(pi_p4_id_t p4_id) const;

  Status validate_action(const p4::v1::Action &action);

  pi_dev_tgt_t device_tgt;
  pi_p4_id_t act_prof_id;
  pi_p4info_t *p4info;
  // set at construction time, cannot be changed during the lifetime of the
  // object
  PiApiChoice pi_api_choice;
  WatchPortEnforcer *watch_port_enforcer;  // non-owning pointer
  size_t max_group_size{0};
};


class ActionProfAccessManual : public ActionProfAccessBase {
 public:
  using Id = ActionProfBiMap::Id;

  // inheriting base constructor to reduce boilerplate code
  using ActionProfAccessBase::ActionProfAccessBase;

  Status member_create(const p4::v1::ActionProfileMember &member,
                       const SessionTemp &session);

  Status group_create(const p4::v1::ActionProfileGroup &group,
                      const SessionTemp &session);

  Status member_modify(const p4::v1::ActionProfileMember &member,
                       const SessionTemp &session);

  Status group_modify(const p4::v1::ActionProfileGroup &group,
                      const SessionTemp &session);

  Status member_delete(const p4::v1::ActionProfileMember &member,
                       const SessionTemp &session);

  Status group_delete(const p4::v1::ActionProfileGroup &group,
                      const SessionTemp &session);

  bool group_get_max_size_user(const Id &group_id, size_t *max_size_user) const;

  bool get_member_info(const Id &group_id, const Id &member_id,
                       int *weight, WatchPort *watch_port) const;

  // would be nice to be able to use boost::optional for the retrieve functions;
  // we cannot return a pointer (that would be null if the key couldn't be
  // found) because some other thread may come in and remove the corresponding
  // group / member, thus invalidating the pointer.

  // returns false if no matching id
  bool retrieve_member_handle(const Id &member_id,
                              pi_indirect_handle_t *member_h) const;
  bool retrieve_group_handle(const Id &group_id,
                             pi_indirect_handle_t *group_h) const;

  // returns false if no matching handle
  bool retrieve_member_id(pi_indirect_handle_t member_h, Id *member_id) const;
  bool retrieve_group_id(pi_indirect_handle_t group_h, Id *group_id) const;

 private:
  bool empty() const override;

  StatusOr<size_t> validate_max_group_size(int max_size);

  Status group_update_members(pi::ActProf &ap,  // NOLINT(runtime/references)
                              const p4::v1::ActionProfileGroup &group);

  Status create_missing_weighted_members(
      pi::ActProf &ap,  // NOLINT(runtime/references)
      ActionProfMemberMap::MemberState *member_state,
      const ActionProfGroupMembership::MembershipUpdate &update);

  Status purge_unused_weighted_members(
      pi::ActProf &ap,  // NOLINT(runtime/references)
      ActionProfMemberMap::MemberState *member_state);

  // gives "critical" error if purge_unused_weighted_members fails
  Status purge_unused_weighted_members_wrapper(
      pi::ActProf &ap,  // NOLINT(runtime/references)
      ActionProfMemberMap::MemberState *member_state);

  ActionProfMemberMap member_map;
  ActionProfBiMap group_bimap{};
  std::map<Id, ActionProfGroupMembership> group_members{};
};

class ActionProfAccessOneshot : public ActionProfAccessBase {
 public:
  // inheriting base constructor to reduce boilerplate code
  using ActionProfAccessBase::ActionProfAccessBase;

  Status group_create(const p4::v1::ActionProfileActionSet &action_set,
                      pi_indirect_handle_t *group_h, SessionTemp *session);
  Status group_delete(pi_indirect_handle_t group_h, const SessionTemp &session);

  struct OneShotMember {
    pi_indirect_handle_t member_h;
    // When a one-shot is created with weights > 1, we create multiple member
    // copies and add them to the group. In order to respect read-write
    // symmetry, we need to remember weight information. In the vector of
    // OneShotMember that we store for each one-shot group, the first copy
    // stores the correct, user-provided weight, while all the subsequent copies
    // have their weight field set to 0.
    int weight;
    WatchPort watch;
  };

  bool group_get_members(pi_indirect_handle_t group_h,
                         std::vector<OneShotMember> *members) const;

 private:
  // nested classes so they have access to private data members and can create a
  // pi::ActProf instance.
  struct OneShotGroupCleanupTask;
  struct OneShotMemberCleanupTask;
  struct OneShotWatchPortCleanupTask;

  Status group_create_helper(
      pi::ActProf &ap,  // NOLINT(runtime/references)
      pi_indirect_handle_t group_h,
      std::vector<pi_indirect_handle_t> members_h,
      std::vector<pi_port_t> members_watch_port,
      SessionTemp *session);

  bool empty() const override;

  std::unordered_map<pi_indirect_handle_t, std::vector<OneShotMember> >
  group_members{};
};

class ActionProfMgr {
 public:
  using PiApiChoice = ActionProfAccessBase::PiApiChoice;
  using SelectorUsage = ActionProfAccessBase::SelectorUsage;

  ActionProfMgr(pi_dev_tgt_t device_tgt, pi_p4_id_t act_prof_id,
                pi_p4info_t *p4info, PiApiChoice pi_api_choice,
                WatchPortEnforcer *watch_port_enforcer);

  StatusOr<ActionProfAccessOneshot *> oneshot();

  StatusOr<ActionProfAccessManual *> manual();

  SelectorUsage get_selector_usage() const {
    return selector_usage;
  }

  // Choose the best programming style (individual adds / removes, or set
  // membership) for the target.
  static StatusOr<PiApiChoice> choose_pi_api(pi_dev_id_t device_id);

 private:
  template <typename T>
  Status check_selector_usage();

  SelectorUsage selector_usage{SelectorUsage::UNSPECIFIED};
  pi_dev_tgt_t device_tgt;
  pi_p4_id_t act_prof_id;
  pi_p4info_t *p4info;
  // set at construction time, cannot be changed during the lifetime of the
  // object
  PiApiChoice pi_api_choice;
  WatchPortEnforcer *watch_port_enforcer;  // non-owning pointer
  std::unique_ptr<ActionProfAccessBase> pimp;
};

}  // namespace proto

}  // namespace fe

}  // namespace pi

#endif  // SRC_ACTION_PROF_MGR_H_
