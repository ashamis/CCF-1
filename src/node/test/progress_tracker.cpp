// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.

#include "node/progress_tracker.h"

#include "consensus/aft/impl/view_change_tracker.h"
#include "kv/store.h"
#include "node/nodes.h"
#include "node/request_tracker.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <trompeloeil/include/trompeloeil.hpp>

class StoreMock : public ccf::ProgressTrackerStore
{
public:
  MAKE_MOCK1(write_backup_signatures, void(ccf::BackupSignatures&), override);
  MAKE_MOCK0(
    get_backup_signatures, std::optional<ccf::BackupSignatures>(), override);
  MAKE_MOCK0(
    get_new_view, std::optional<ccf::ViewChangeConfirmation>(), override);
  MAKE_MOCK1(write_nonces, void(aft::RevealedNonces&), override);
  MAKE_MOCK0(get_nonces, std::optional<aft::RevealedNonces>(), override);
  MAKE_MOCK4(
    verify_signature,
    bool(kv::NodeId, crypto::Sha256Hash&, uint32_t, uint8_t*),
    override);
  MAKE_MOCK3(
    sign_view_change_request,
    void(
      ccf::ViewChangeRequest& view_change,
      kv::Consensus::View view,
      kv::Consensus::SeqNo seqno),
    override);
  MAKE_MOCK4(
    verify_view_change_request,
    bool(
      ccf::ViewChangeRequest& view_change,
      kv::NodeId from,
      kv::Consensus::View view,
      kv::Consensus::SeqNo seqno),
    override);
  MAKE_MOCK2(
    verify_view_change_request_confirmation,
    bool(ccf::ViewChangeConfirmation& new_view, kv::NodeId from),
    override);
  MAKE_MOCK1(
    write_view_change_confirmation,
    kv::Consensus::SeqNo(ccf::ViewChangeConfirmation& new_view),
    override);
};

void ordered_execution(
  uint32_t my_node_id, std::unique_ptr<ccf::ProgressTracker>& pt)
{
  kv::Consensus::View view = 0;
  kv::Consensus::SeqNo seqno = 42;
  uint32_t node_count = 4;
  uint32_t node_count_quorum =
    2; // Takes into account that counting starts at 0
  bool am_i_primary = (my_node_id == 0);

  crypto::Sha256Hash root;
  std::array<uint8_t, MBEDTLS_ECDSA_MAX_LEN> sig;
  ccf::Nonce nonce;
  auto h = pt->hash_data(nonce);
  ccf::Nonce hashed_nonce;
  std::copy(h.h.begin(), h.h.end(), hashed_nonce.h.begin());
  std::vector<uint8_t> primary_sig;

  INFO("Adding signatures");
  {
    auto result = pt->record_primary(
      {view, seqno}, 0, root, primary_sig, hashed_nonce, node_count);
    REQUIRE(result == kv::TxHistory::Result::OK);
    primary_sig = {1};
    result = pt->record_primary_signature({view, seqno}, primary_sig);
    REQUIRE(result == kv::TxHistory::Result::OK);

    for (uint32_t i = 1; i < node_count; ++i)
    {
      if (i == my_node_id)
      {
        auto h = pt->get_my_hashed_nonce({view, seqno});
        std::copy(h.h.begin(), h.h.end(), hashed_nonce.h.begin());
      }
      else
      {
        std::copy(h.h.begin(), h.h.end(), hashed_nonce.h.begin());
      }

      auto result = pt->add_signature(
        {view, seqno},
        i,
        MBEDTLS_ECDSA_MAX_LEN,
        sig,
        hashed_nonce,
        node_count,
        am_i_primary);
      REQUIRE(
        ((result == kv::TxHistory::Result::OK && i != node_count_quorum) ||
         (result == kv::TxHistory::Result::SEND_SIG_RECEIPT_ACK &&
          i == node_count_quorum)));
    }
  }

  INFO("Add signature acks");
  {
    for (uint32_t i = 0; i < node_count; ++i)
    {
      auto result = pt->add_signature_ack({view, seqno}, i, node_count);
      REQUIRE(
        ((result == kv::TxHistory::Result::OK && i != node_count_quorum) ||
         (result == kv::TxHistory::Result::SEND_REPLY_AND_NONCE &&
          i == node_count_quorum)));
    }
  }

  INFO("Add nonces here");
  {
    for (uint32_t i = 0; i < node_count; ++i)
    {
      if (my_node_id == i)
      {
        pt->add_nonce_reveal(
          {view, seqno},
          pt->get_my_nonce({view, seqno}),
          i,
          node_count,
          am_i_primary);
      }
      else
      {
        pt->add_nonce_reveal({view, seqno}, nonce, i, node_count, am_i_primary);
      }

      if (i < 2)
      {
        REQUIRE(pt->get_highest_committed_nonce() == 0);
      }
      else
      {
        REQUIRE(pt->get_highest_committed_nonce() == seqno);
      }
    }
  }
}

void ordered_execution_primary(
  uint32_t my_node_id,
  std::unique_ptr<ccf::ProgressTracker> pt,
  StoreMock& store_mock)
{
  using trompeloeil::_;

  REQUIRE_CALL(store_mock, write_backup_signatures(_));
  REQUIRE_CALL(store_mock, write_nonces(_));

  ordered_execution(my_node_id, pt);
}

void run_ordered_execution(uint32_t my_node_id)
{
  using trompeloeil::_;

  auto store = std::make_unique<StoreMock>();
  StoreMock& store_mock = *store.get();
  auto pt =
    std::make_unique<ccf::ProgressTracker>(std::move(store), my_node_id);

  REQUIRE_CALL(store_mock, verify_signature(_, _, _, _))
    .RETURN(true)
    .TIMES(AT_LEAST(2));

  if (my_node_id == 0)
  {
    ordered_execution_primary(my_node_id, std::move(pt), store_mock);
  }
  else
  {
    ordered_execution(my_node_id, pt);
  }
}

TEST_CASE("Ordered Execution")
{
  for (uint32_t i = 0; i < 4; ++i)
  {
    run_ordered_execution(i);
  }
}

TEST_CASE("Request tracker")
{
  INFO("Can add and remove from progress tracker");
  {
    aft::RequestTracker t;
    crypto::Sha256Hash h;
    h.h.fill(0);
    for (uint32_t i = 0; i < 10; ++i)
    {
      h.h[0] = i;
      t.insert(h, std::chrono::milliseconds(i));
      REQUIRE(t.oldest_entry() == std::chrono::milliseconds(0));
    }

    h.h[0] = 2;
    REQUIRE(t.remove(h));
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(0));

    h.h[0] = 0;
    REQUIRE(t.remove(h));
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(1));

    h.h[0] = 99;
    REQUIRE(t.remove(h) == false);
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(1));
  }

  INFO("Entry that was deleted is not tracked after it is added");
  {
    aft::RequestTracker t;
    crypto::Sha256Hash h;
    h.h.fill(0);
    REQUIRE(t.oldest_entry().has_value() == false);

    h.h[0] = 0;
    REQUIRE(t.remove(h) == false);
    t.insert_deleted(h, std::chrono::milliseconds(100));
    t.insert(h, std::chrono::milliseconds(0));
    REQUIRE(t.oldest_entry().has_value() == false);

    h.h[1] = 1;
    REQUIRE(t.remove(h) == false);
    t.insert_deleted(h, std::chrono::milliseconds(100));
    t.tick(std::chrono::milliseconds(120));
    t.insert(h, std::chrono::milliseconds(0));
    REQUIRE(t.oldest_entry().has_value() == false);

    h.h[2] = 2;
    REQUIRE(t.remove(h) == false);
    t.insert_deleted(h, std::chrono::milliseconds(100));
    t.tick(std::chrono::minutes(3));
    REQUIRE(t.is_empty());
    t.insert(h, std::chrono::milliseconds(0));
    REQUIRE(t.oldest_entry().has_value());
  }

  INFO("Can enter multiple items");
  {
    aft::RequestTracker t;
    crypto::Sha256Hash h;
    h.h.fill(0);

    t.insert(h, std::chrono::milliseconds(0));

    for (uint32_t i = 1; i < 4; ++i)
    {
      h.h[0] = 1;
      t.insert(h, std::chrono::milliseconds(i));
    }

    h.h[0] = 2;
    t.insert(h, std::chrono::milliseconds(4));
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(0));

    h.h[0] = 1;
    REQUIRE(t.remove(h));
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(0));

    h.h[0] = 0;
    t.remove(h);
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(2));

    h.h[0] = 1;
    t.remove(h);
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(3));
    t.remove(h);
    REQUIRE(t.oldest_entry() == std::chrono::milliseconds(4));
    t.remove(h);
    REQUIRE(!t.is_empty());

    h.h[0] = 2;
    t.remove(h);
    REQUIRE(t.is_empty());
  }

  INFO("Verify seqno and time of last sig stored correctly");
  {
    aft::RequestTracker t;

    auto r = t.get_seqno_time_last_request();
    REQUIRE(std::get<0>(r) == -1);
    REQUIRE(std::get<1>(r) == std::chrono::milliseconds(0));

    t.insert_signed_request(2, std::chrono::milliseconds(2));
    r = t.get_seqno_time_last_request();
    REQUIRE(std::get<0>(r) == 2);
    REQUIRE(std::get<1>(r) == std::chrono::milliseconds(2));

    t.insert_signed_request(1, std::chrono::milliseconds(1));
    r = t.get_seqno_time_last_request();
    REQUIRE(std::get<0>(r) == 2);
    REQUIRE(std::get<1>(r) == std::chrono::milliseconds(2));
  }
}

TEST_CASE("Record primary signature")
{
  uint32_t my_node_id = 0;
  kv::Consensus::View view = 0;
  kv::Consensus::SeqNo seqno = 42;
  crypto::Sha256Hash root;
  ccf::Nonce nonce;
  std::vector<uint8_t> primary_sig;

  ccf::ProgressTracker pt(nullptr, my_node_id);

  auto result = pt.record_primary({view, seqno}, 0, root, primary_sig, nonce);
  REQUIRE(result == kv::TxHistory::Result::OK);

  primary_sig = {1};
  result = pt.record_primary_signature({view, seqno}, primary_sig);
  REQUIRE(result == kv::TxHistory::Result::OK);
  result = pt.record_primary_signature({view, seqno + 1}, primary_sig);
  REQUIRE(result != kv::TxHistory::Result::OK);
}

TEST_CASE("View Changes")
{
  using trompeloeil::_;

  uint32_t my_node_id = 0;
  auto store = std::make_unique<StoreMock>();
  StoreMock& store_mock = *store.get();
  ccf::ProgressTracker pt(std::move(store), my_node_id);

  kv::Consensus::View view = 0;
  kv::Consensus::SeqNo seqno = 42;
  uint32_t node_count = 4;
  uint32_t node_count_quorum =
    2; // Takes into account that counting starts at 0
  crypto::Sha256Hash root;
  root.h.fill(1);
  ccf::Nonce nonce;
  auto h = pt.hash_data(nonce);
  ccf::Nonce hashed_nonce;
  std::copy(h.h.begin(), h.h.end(), hashed_nonce.h.begin());
  std::array<uint8_t, MBEDTLS_ECDSA_MAX_LEN> sig;
  std::vector<uint8_t> primary_sig;

  INFO("find first view-change message");
  {
    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _))
      .RETURN(true)
      .TIMES(AT_LEAST(2));
    REQUIRE_CALL(store_mock, sign_view_change_request(_, _, _))
      .TIMES(AT_LEAST(2));
    auto result = pt.record_primary(
      {view, seqno}, 0, root, primary_sig, hashed_nonce, node_count);
    REQUIRE(result == kv::TxHistory::Result::OK);

    for (uint32_t i = 1; i < node_count; ++i)
    {
      auto result = pt.add_signature(
        {view, seqno},
        i,
        MBEDTLS_ECDSA_MAX_LEN,
        sig,
        hashed_nonce,
        node_count,
        false);
      REQUIRE(
        ((result == kv::TxHistory::Result::OK && i != node_count_quorum) ||
         (result == kv::TxHistory::Result::SEND_SIG_RECEIPT_ACK &&
          i == node_count_quorum)));

      if (i < 2)
      {
        CHECK_THROWS(pt.get_view_change_message(view));
      }
      else
      {
        auto vc = pt.get_view_change_message(view);
        REQUIRE(std::get<0>(vc) != nullptr);
      }
    }
  }

  INFO("Update latest prepared");
  {
    kv::Consensus::SeqNo new_seqno = 84;

    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _))
      .RETURN(true)
      .TIMES(AT_LEAST(2));
    REQUIRE_CALL(store_mock, sign_view_change_request(_, _, _))
      .TIMES(AT_LEAST(2));
    auto result = pt.record_primary(
      {view, new_seqno}, 0, root, primary_sig, hashed_nonce, node_count);
    REQUIRE(result == kv::TxHistory::Result::OK);

    for (uint32_t i = 1; i < node_count; ++i)
    {
      auto result = pt.add_signature(
        {view, new_seqno},
        i,
        MBEDTLS_ECDSA_MAX_LEN,
        sig,
        hashed_nonce,
        node_count,
        false);
      REQUIRE(
        ((result == kv::TxHistory::Result::OK && i != node_count_quorum) ||
         (result == kv::TxHistory::Result::SEND_SIG_RECEIPT_ACK &&
          i == node_count_quorum)));

      if (i < 2)
      {
        auto vc = pt.get_view_change_message(view);
        REQUIRE(std::get<0>(vc) != nullptr);
      }
      else
      {
        auto vc = pt.get_view_change_message(view);
        REQUIRE(std::get<0>(vc) != nullptr);
      }
    }
    seqno = new_seqno;
  }

  INFO("Update older prepared");
  {
    kv::Consensus::SeqNo new_seqno = 21;

    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _))
      .RETURN(true)
      .TIMES(AT_LEAST(2));
    REQUIRE_CALL(store_mock, sign_view_change_request(_, _, _))
      .TIMES(AT_LEAST(2));
    auto result = pt.record_primary(
      {view, new_seqno}, 0, root, primary_sig, hashed_nonce, node_count);
    REQUIRE(result == kv::TxHistory::Result::OK);

    for (uint32_t i = 1; i < node_count; ++i)
    {
      auto result = pt.add_signature(
        {view, new_seqno},
        i,
        MBEDTLS_ECDSA_MAX_LEN,
        sig,
        hashed_nonce,
        node_count,
        false);
      REQUIRE(
        ((result == kv::TxHistory::Result::OK && i != node_count_quorum) ||
         (result == kv::TxHistory::Result::SEND_SIG_RECEIPT_ACK &&
          i == node_count_quorum)));

      auto vc = pt.get_view_change_message(view);
      REQUIRE(std::get<0>(vc) != nullptr);
    }
  }
}

TEST_CASE("Serialization")
{
  std::vector<uint8_t> serialized;
  INFO("view-change serialization");
  {
    ccf::ViewChangeRequest v;

    for (uint32_t i = 10; i < 110; i += 10)
    {
      ccf::Nonce n;
      n.h.fill(i + 2);
      v.signatures.push_back({{static_cast<uint8_t>(i)}, i + 1, n});
    }

    v.signature = {5};
    serialized.resize(v.get_serialized_size());

    uint8_t* data = serialized.data();
    size_t size = serialized.size();

    v.serialize(data, size);
    REQUIRE(size == 0);
  }

  INFO("view-change deserialization");
  {
    const uint8_t* data = serialized.data();
    size_t size = serialized.size();
    ccf::ViewChangeRequest v = ccf::ViewChangeRequest::deserialize(data, size);

    REQUIRE(v.signatures.size() == 10);
    for (uint32_t i = 1; i < 11; ++i)
    {
      ccf::Nonce n;
      n.h.fill(i * 10 + 2);
      ccf::NodeSignature& ns = v.signatures[i - 1];
      REQUIRE(ns.sig.size() == 1);
      REQUIRE(ns.sig[0] == i * 10);
      REQUIRE(ns.node == i * 10 + 1);
      REQUIRE(ns.hashed_nonce.h == n.h);
    }

    REQUIRE(v.signature.size() == 1);
    REQUIRE(v.signature[0] == 5);
  }
}

TEST_CASE("view-change-tracker timeout tests")
{
  INFO("Check timeout works correctly");
  {
    aft::ViewChangeTracker vct(nullptr, std::chrono::seconds(10));
    REQUIRE(vct.should_send_view_change(std::chrono::seconds(1)) == false);
    REQUIRE(vct.get_target_view() == 0);
    REQUIRE(vct.should_send_view_change(std::chrono::seconds(11)));
    REQUIRE(vct.get_target_view() == 1);
    REQUIRE(vct.should_send_view_change(std::chrono::seconds(12)) == false);
    REQUIRE(vct.get_target_view() == 1);
    REQUIRE(vct.should_send_view_change(std::chrono::seconds(100)));
    REQUIRE(vct.get_target_view() == 2);
  }
}

TEST_CASE("view-change-tracker statemachine tests")
{
  ccf::ViewChangeRequest v;
  kv::Consensus::View view = 3;
  kv::Consensus::SeqNo seqno = 1;
  uint32_t node_count = 4;

  INFO("Can trigger view change");
  {
    aft::ViewChangeTracker vct(nullptr, std::chrono::seconds(10));
    for (uint32_t i = 0; i < node_count; ++i)
    {
      auto r = vct.add_request_view_change(v, i, view, seqno, node_count);
      if (i == 2)
      {
        REQUIRE(
          r == aft::ViewChangeTracker::ResultAddView::APPEND_NEW_VIEW_MESSAGE);
      }
      else
      {
        REQUIRE(r == aft::ViewChangeTracker::ResultAddView::OK);
      }
      REQUIRE((vct.check_evidence(view) == i >= 2));
      REQUIRE(!vct.check_evidence(view + 1));
    }
    vct.clear(true, view);
    REQUIRE(vct.check_evidence(view));
    REQUIRE(!vct.check_evidence(view + 1));
  }

  INFO("Can differentiate view change for different view");
  {
    aft::ViewChangeTracker vct(nullptr, std::chrono::seconds(10));
    for (uint32_t i = 0; i < node_count; ++i)
    {
      auto r = vct.add_request_view_change(v, i, i, seqno, node_count);
      REQUIRE(r == aft::ViewChangeTracker::ResultAddView::OK);
    }
  }
}

TEST_CASE("test progress_tracker apply_view_change")
{
  using trompeloeil::_;

  uint32_t node_id = 1;
  auto store = std::make_unique<StoreMock>();
  StoreMock& store_mock = *store.get();
  auto pt = std::make_unique<ccf::ProgressTracker>(std::move(store), node_id);

  {
    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _))
      .RETURN(true)
      .TIMES(AT_LEAST(2));

    ordered_execution(node_id, pt);
  }

  INFO("View-change signature does not verify");
  {
    REQUIRE_CALL(store_mock, verify_view_change_request(_, _, _, _))
      .RETURN(false);
    ccf::ViewChangeRequest v;
    bool result = pt->apply_view_change_message(v, 1, 1, 1);
    REQUIRE(result == false);
  }

  INFO("Unknown seqno");
  {
    REQUIRE_CALL(store_mock, verify_view_change_request(_, _, _, _))
      .RETURN(true);
    ccf::ViewChangeRequest v;
    bool result = pt->apply_view_change_message(v, 1, 1, 999);
    REQUIRE(result == false);
  }

  INFO("View-change matches - known node");
  {
    REQUIRE_CALL(store_mock, verify_view_change_request(_, _, _, _))
      .RETURN(true);
    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _)).RETURN(true);
    ccf::ViewChangeRequest v;
    v.signatures.push_back(ccf::NodeSignature(0));

    bool result = pt->apply_view_change_message(v, 1, 1, 42);
    REQUIRE(result);
  }

  INFO("View-change matches - unknown node");
  {
    REQUIRE_CALL(store_mock, verify_view_change_request(_, _, _, _))
      .RETURN(true);
    REQUIRE_CALL(store_mock, verify_signature(_, _, _, _)).RETURN(false);

    ccf::ViewChangeRequest v;
    v.signatures.push_back(ccf::NodeSignature(5));

    bool result = pt->apply_view_change_message(v, 1, 1, 42);
    REQUIRE(result == false);
  }
}

TEST_CASE("Sending evidence out of band")
{
  using trompeloeil::_;

  ccf::ViewChangeRequest v;
  kv::Consensus::View view = 3;
  kv::Consensus::SeqNo seqno = 1;
  constexpr uint32_t node_count = 4;

  INFO("Can trigger view change");
  {
    aft::ViewChangeTracker vct(nullptr, std::chrono::seconds(10));
    for (uint32_t i = 0; i < node_count; ++i)
    {
      auto r = vct.add_request_view_change(v, i, view, seqno, node_count);
      if (i == 2)
      {
        REQUIRE(
          r == aft::ViewChangeTracker::ResultAddView::APPEND_NEW_VIEW_MESSAGE);
      }
      else
      {
        REQUIRE(r == aft::ViewChangeTracker::ResultAddView::OK);
      }

      auto data = vct.get_serialized_view_change_confirmation(view);
      std::shared_ptr<ccf::ProgressTrackerStore> store =
        std::make_unique<StoreMock>();

      aft::ViewChangeTracker vct_2(store, std::chrono::seconds(10));
      if (i >= 2)
      {
        REQUIRE_CALL(
          *reinterpret_cast<StoreMock*>(store.get()),
          verify_view_change_request(_, _, _, _))
          .RETURN(true)
          .TIMES(AT_LEAST(1));

        REQUIRE(vct_2.add_unknown_primary_evidence(
          {data.data(), data.size()}, view, node_count));
        REQUIRE(vct_2.check_evidence(view));
      }
      else
      {
        REQUIRE(!vct_2.add_unknown_primary_evidence(
          {data.data(), data.size()}, view, node_count));
        REQUIRE(!vct_2.check_evidence(view));
      }
      REQUIRE(!vct_2.check_evidence(view + 1));
    }
  }
}