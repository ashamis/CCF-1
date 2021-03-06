// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../ledger.h"

#include "../ds/serialized.h"

#include <doctest/doctest.h>
#include <string>

using namespace asynchost;

// Used throughout
using frame_header_type = uint32_t;
static constexpr size_t frame_header_size = sizeof(frame_header_type);
static constexpr auto ledger_dir = "ledger_dir";

constexpr auto buffer_size = 1024;
auto in_buffer = std::make_unique<ringbuffer::TestBuffer>(buffer_size);
auto out_buffer = std::make_unique<ringbuffer::TestBuffer>(buffer_size);
ringbuffer::Circuit eio(in_buffer->bd, out_buffer->bd);

auto wf = ringbuffer::WriterFactory(eio);

// Ledger entry type
template <typename T>
struct LedgerEntry
{
  T value_ = 0;

  uint8_t* data()
  {
    return reinterpret_cast<uint8_t*>(&value_);
  }

  auto value() const
  {
    return value_;
  }

  auto set_value(T v)
  {
    value_ = v;
  }

  LedgerEntry() = default;
  LedgerEntry(T v) : value_(v) {}
  LedgerEntry(const std::vector<uint8_t>& raw)
  {
    const uint8_t* data = raw.data();
    size_t size = raw.size();
    value_ = serialized::read<T>(data, size);
  }
};
using TestLedgerEntry = LedgerEntry<uint32_t>;

size_t number_of_files_in_ledger_dir()
{
  size_t file_count = 0;
  for (auto const& f : fs::directory_iterator(ledger_dir))
  {
    file_count++;
  }
  return file_count;
}

size_t number_of_committed_files_in_ledger_dir()
{
  size_t committed_file_count = 0;
  for (auto const& f : fs::directory_iterator(ledger_dir))
  {
    if (is_ledger_file_committed(f.path().string()))
    {
      committed_file_count++;
    }
  }

  return committed_file_count;
}

void verify_framed_entries_range(
  const std::vector<uint8_t>& framed_entries, size_t from, size_t to)
{
  size_t idx = from;
  for (int i = 0; i < framed_entries.size();)
  {
    const uint8_t* data = &framed_entries[i];
    size_t size = framed_entries.size() - i;

    auto frame = serialized::read<frame_header_type>(data, size);
    auto entry = serialized::read(data, size, frame);
    REQUIRE(TestLedgerEntry(entry).value() == idx);
    i += frame_header_size + frame;
    idx++;
  }

  REQUIRE(idx == to + 1);
}

void read_entry_from_ledger(Ledger& ledger, size_t idx)
{
  REQUIRE(TestLedgerEntry(ledger.read_entry(idx).value()).value() == idx);
}

void read_entries_range_from_ledger(Ledger& ledger, size_t from, size_t to)
{
  verify_framed_entries_range(
    ledger.read_framed_entries(from, to).value(), from, to);
}

// Keeps track of ledger entries written to the ledger.
// An entry submitted at index i has for value i so that it is easy to verify
// that the ledger entry read from the ledger at a specific index is right.
class TestEntrySubmitter
{
private:
  Ledger& ledger;
  size_t last_idx;

public:
  TestEntrySubmitter(Ledger& ledger, size_t initial_last_idx = 0) :
    ledger(ledger),
    last_idx(initial_last_idx)
  {}

  size_t get_last_idx()
  {
    return last_idx;
  }

  void write(bool is_committable, bool force_chunk = false)
  {
    auto e = TestLedgerEntry(++last_idx);
    REQUIRE(
      ledger.write_entry(
        e.data(), sizeof(TestLedgerEntry), is_committable, force_chunk) ==
      last_idx);
  }

  void truncate(size_t idx)
  {
    ledger.truncate(idx);

    // Check that we can read until truncated entry but cannot read after it
    if (idx > 0)
    {
      read_entries_range_from_ledger(ledger, 1, idx);
    }
    REQUIRE_FALSE(ledger.read_framed_entries(1, idx + 1).has_value());

    if (idx < last_idx)
    {
      last_idx = idx;
    }
  }
};

size_t get_entries_per_chunk(size_t chunk_threshold)
{
  // The number of entries per chunk is a function of the threshold (minus the
  // size of the fixes space for the offset at the size of each file) and the
  // size of each _framed_ entry
  return ceil(
    (static_cast<float>(chunk_threshold - sizeof(size_t))) /
    (frame_header_size + sizeof(TestLedgerEntry)));
}

// Assumes that no entries have been written yet
size_t initialise_ledger(
  TestEntrySubmitter& entry_submitter,
  size_t chunk_threshold,
  size_t chunk_count)
{
  size_t end_of_first_chunk_idx = 0;
  bool is_committable = true;
  size_t entries_per_chunk = get_entries_per_chunk(chunk_threshold);

  for (int i = 0; i < entries_per_chunk * chunk_count; i++)
  {
    entry_submitter.write(is_committable);
  }

  REQUIRE(number_of_files_in_ledger_dir() == chunk_count);

  return entries_per_chunk;
}

TEST_CASE("Regular chunking")
{
  fs::remove_all(ledger_dir);

  INFO("Cannot create a ledger with a chunk threshold of 0");
  {
    size_t chunk_threshold = 0;
    REQUIRE_THROWS(Ledger(ledger_dir, wf, chunk_threshold));
  }

  size_t chunk_threshold = 30;
  size_t entries_per_chunk = get_entries_per_chunk(chunk_threshold);
  Ledger ledger(ledger_dir, wf, chunk_threshold);
  TestEntrySubmitter entry_submitter(ledger);

  size_t end_of_first_chunk_idx = 0;
  bool is_committable = true;

  INFO("Not quite enough entries before chunk threshold");
  {
    is_committable = true;
    for (int i = 0; i < entries_per_chunk - 1; i++)
    {
      entry_submitter.write(is_committable);
    }

    // Writing committable entries without reaching the chunk threshold
    // does not create new ledger files
    REQUIRE(number_of_files_in_ledger_dir() == 1);
  }

  INFO("Additional non-committable entries do not trigger chunking");
  {
    is_committable = false;
    entry_submitter.write(is_committable);
    entry_submitter.write(is_committable);
    REQUIRE(number_of_files_in_ledger_dir() == 1);
  }

  INFO("Additional committable entry triggers chunking");
  {
    is_committable = true;
    entry_submitter.write(is_committable);
    REQUIRE(number_of_files_in_ledger_dir() == 1);

    // Threshold is passed, a new ledger file should be created
    entry_submitter.write(false);
    end_of_first_chunk_idx = entry_submitter.get_last_idx() - 1;
    REQUIRE(number_of_files_in_ledger_dir() == 2);
  }

  INFO(
    "Submitting more committable entries trigger chunking at regular interval");
  {
    size_t chunk_count = 10;
    size_t number_of_files_before = number_of_files_in_ledger_dir();
    for (int i = 0; i < entries_per_chunk * chunk_count; i++)
    {
      entry_submitter.write(is_committable);
    }
    REQUIRE(
      number_of_files_in_ledger_dir() == chunk_count + number_of_files_before);
  }

  INFO("Forcing early chunk from a committable entry");
  {
    size_t number_of_files_before = number_of_files_in_ledger_dir();

    // Write committable entries until a new chunk with one entry is created
    is_committable = true;
    while (number_of_files_in_ledger_dir() == number_of_files_before)
    {
      entry_submitter.write(is_committable);
    }

    size_t number_of_files_after = number_of_files_in_ledger_dir();

    // Write a new committable entry that forces a new ledger chunk
    is_committable = true;
    bool force_new_chunk = true;
    entry_submitter.write(is_committable, force_new_chunk);
    REQUIRE(number_of_files_in_ledger_dir() == number_of_files_after);

    // Because of forcing a new chunk, the next entry will create a new chunk
    is_committable = false;
    entry_submitter.write(is_committable);

    // A new chunk is created as the entry is committable _and_ forced
    REQUIRE(number_of_files_in_ledger_dir() == number_of_files_after + 1);

    is_committable = true;
    force_new_chunk = true;
    entry_submitter.write(is_committable, force_new_chunk);
    // No new chunk is created as the entry is committable but doesn't force a
    // new chunk
    REQUIRE(number_of_files_in_ledger_dir() == number_of_files_after + 1);
  }

  INFO("Reading entries across all chunks");
  {
    is_committable = false;
    entry_submitter.write(is_committable);
    auto last_idx = entry_submitter.get_last_idx();

    // Reading the last entry succeeds
    read_entry_from_ledger(ledger, last_idx);

    // Reading in the future fails
    REQUIRE_FALSE(ledger.read_entry(last_idx + 1).has_value());

    // Reading at 0 fails
    REQUIRE_FALSE(ledger.read_entry(0).has_value());

    // Reading in the past succeeds
    read_entry_from_ledger(ledger, 1);
    read_entry_from_ledger(ledger, end_of_first_chunk_idx);
    read_entry_from_ledger(ledger, end_of_first_chunk_idx + 1);
    read_entry_from_ledger(ledger, last_idx);
  }

  INFO("Reading range of entries across all chunks");
  {
    // Note: only testing write cache as no chunk has yet been committed
    auto last_idx = entry_submitter.get_last_idx();

    // Reading from 0 fails
    REQUIRE_FALSE(
      ledger.read_framed_entries(0, end_of_first_chunk_idx).has_value());

    // Reading in the future fails
    REQUIRE_FALSE(ledger.read_framed_entries(1, last_idx + 1).has_value());
    REQUIRE_FALSE(
      ledger.read_framed_entries(last_idx, last_idx + 1).has_value());

    // Reading from the start to any valid index succeeds
    read_entries_range_from_ledger(ledger, 1, 1);
    read_entries_range_from_ledger(
      ledger, end_of_first_chunk_idx - 1, end_of_first_chunk_idx);
    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx);
    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx + 1);
    read_entries_range_from_ledger(ledger, 1, last_idx - 1);
    read_entries_range_from_ledger(ledger, 1, last_idx);

    // Reading from just before/after a chunk succeeds
    read_entries_range_from_ledger(
      ledger, end_of_first_chunk_idx, end_of_first_chunk_idx + 1);
    read_entries_range_from_ledger(
      ledger, end_of_first_chunk_idx, last_idx - 1);
    read_entries_range_from_ledger(ledger, end_of_first_chunk_idx, last_idx);
    read_entries_range_from_ledger(
      ledger, end_of_first_chunk_idx + 1, last_idx);
    read_entries_range_from_ledger(
      ledger, end_of_first_chunk_idx + 1, last_idx - 1);
  }
}

TEST_CASE("Truncation")
{
  fs::remove_all(ledger_dir);

  size_t chunk_threshold = 30;
  Ledger ledger(ledger_dir, wf, chunk_threshold);
  TestEntrySubmitter entry_submitter(ledger);

  size_t chunk_count = 3;
  size_t end_of_first_chunk_idx =
    initialise_ledger(entry_submitter, chunk_threshold, chunk_count);

  // Write another entry to create a new chunk
  entry_submitter.write(true);

  size_t chunks_so_far = number_of_files_in_ledger_dir();
  auto last_idx = entry_submitter.get_last_idx();

  INFO("Truncating latest index has no effect");
  {
    entry_submitter.truncate(last_idx);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far);
  }

  INFO("Truncating last entry in penultimate chunk closes latest file");
  {
    entry_submitter.truncate(last_idx - 1);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far - 1);

    // New file gets open when one more entry gets submitted
    entry_submitter.write(true);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far);
    entry_submitter.write(true);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far);
  }

  INFO("Truncating any entry in penultimate chunk closes latest file");
  {
    entry_submitter.truncate(last_idx - 2);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far - 1);

    // New file gets opened when two more entries are submitted
    entry_submitter.write(true);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far - 1);
    entry_submitter.write(true);
    REQUIRE(number_of_files_in_ledger_dir() == chunks_so_far);
  }

  INFO("Truncating entry at the start of second chunk");
  {
    entry_submitter.truncate(end_of_first_chunk_idx + 1);
    REQUIRE(number_of_files_in_ledger_dir() == 2);
  }

  INFO("Truncating entry at the end of first chunk");
  {
    entry_submitter.truncate(end_of_first_chunk_idx);
    REQUIRE(number_of_files_in_ledger_dir() == 1);
    entry_submitter.write(true);
  }

  INFO("Truncating very first entry");
  {
    entry_submitter.truncate(1);
    REQUIRE(number_of_files_in_ledger_dir() == 1);
  }

  INFO("Truncating all the things");
  {
    entry_submitter.truncate(0);
    REQUIRE(number_of_files_in_ledger_dir() == 0);
    entry_submitter.write(true);
  }
}

TEST_CASE("Commit")
{
  fs::remove_all(ledger_dir);

  size_t chunk_threshold = 30;
  Ledger ledger(ledger_dir, wf, chunk_threshold);
  TestEntrySubmitter entry_submitter(ledger);

  size_t chunk_count = 3;
  size_t end_of_first_chunk_idx =
    initialise_ledger(entry_submitter, chunk_threshold, chunk_count);

  entry_submitter.write(true);
  size_t last_idx = entry_submitter.get_last_idx();
  REQUIRE(number_of_committed_files_in_ledger_dir() == 0);

  INFO("Comitting end of first chunk");
  {
    ledger.commit(end_of_first_chunk_idx);
    REQUIRE(number_of_committed_files_in_ledger_dir() == 1);

    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx + 1);
  }

  INFO("Comitting in the middle on complete chunk");
  {
    ledger.commit(end_of_first_chunk_idx + 1);
    REQUIRE(number_of_committed_files_in_ledger_dir() == 1); // No effect
    ledger.commit(2 * end_of_first_chunk_idx - 1); // No effect
    REQUIRE(number_of_committed_files_in_ledger_dir() == 1);
  }

  INFO("Comitting at the end of a complete chunk");
  {
    ledger.commit(2 * end_of_first_chunk_idx);
    REQUIRE(number_of_committed_files_in_ledger_dir() == 2);
    read_entries_range_from_ledger(ledger, 1, 2 * end_of_first_chunk_idx + 1);
  }

  INFO("Comitting at the end of last complete chunk");
  {
    ledger.commit(last_idx - 1);
    REQUIRE(number_of_committed_files_in_ledger_dir() == 3);
    read_entries_range_from_ledger(ledger, 1, last_idx);
  }

  INFO("Comitting incomplete chunk");
  {
    ledger.commit(last_idx); // No effect
    REQUIRE(number_of_committed_files_in_ledger_dir() == 3);
  }

  INFO("Complete latest chunk and commit");
  {
    entry_submitter.write(true);
    entry_submitter.write(true);
    last_idx = entry_submitter.get_last_idx();
    ledger.commit(last_idx);
    REQUIRE(number_of_committed_files_in_ledger_dir() == 4);
    read_entries_range_from_ledger(ledger, 1, last_idx);
  }

  INFO("Ledger cannot be truncated earlier than commit");
  {
    ledger.truncate(1); // No effect
    read_entries_range_from_ledger(ledger, 1, last_idx);

    ledger.truncate(2 * end_of_first_chunk_idx); // No effect
    read_entries_range_from_ledger(ledger, 1, last_idx);

    // Write and truncate a new entry past commit
    entry_submitter.write(true);
    last_idx = entry_submitter.get_last_idx();
    ledger.truncate(last_idx - 1); // Deletes entry at last_idx
    read_entries_range_from_ledger(ledger, 1, last_idx - 1);
    REQUIRE_FALSE(ledger.read_framed_entries(1, last_idx).has_value());
  }
}

TEST_CASE("Restore existing ledger")
{
  fs::remove_all(ledger_dir);

  size_t chunk_threshold = 30;
  size_t last_idx = 0;
  size_t end_of_first_chunk_idx = 0;
  size_t chunk_count = 3;
  size_t number_of_ledger_files = 0;

  SUBCASE("Restoring uncommitted chunks")
  {
    INFO("Initialise first ledger with complete chunks");
    {
      Ledger ledger(ledger_dir, wf, chunk_threshold);
      TestEntrySubmitter entry_submitter(ledger);

      end_of_first_chunk_idx =
        initialise_ledger(entry_submitter, chunk_threshold, chunk_count);
      number_of_ledger_files = number_of_files_in_ledger_dir();
      last_idx = chunk_count * end_of_first_chunk_idx;
    }

    Ledger ledger2(ledger_dir, wf, chunk_threshold);
    read_entries_range_from_ledger(ledger2, 1, last_idx);

    // Restored ledger can be written to
    TestEntrySubmitter entry_submitter(ledger2, last_idx);
    entry_submitter.write(true);
    // On restore, we write a new file as all restored chunks were complete
    REQUIRE(number_of_files_in_ledger_dir() == number_of_ledger_files + 1);
    entry_submitter.write(true);
    entry_submitter.write(true);

    // Restored ledger can be truncated
    entry_submitter.truncate(end_of_first_chunk_idx + 1);
    entry_submitter.truncate(end_of_first_chunk_idx);
    entry_submitter.truncate(1);
  }

  SUBCASE("Restoring truncated ledger")
  {
    INFO("Initialise first ledger with truncation");
    {
      Ledger ledger(ledger_dir, wf, chunk_threshold);
      TestEntrySubmitter entry_submitter(ledger);

      end_of_first_chunk_idx =
        initialise_ledger(entry_submitter, chunk_threshold, chunk_count);

      entry_submitter.truncate(end_of_first_chunk_idx + 1);
      last_idx = entry_submitter.get_last_idx();
      number_of_ledger_files = number_of_files_in_ledger_dir();
    }

    Ledger ledger2(ledger_dir, wf, chunk_threshold);
    read_entries_range_from_ledger(ledger2, 1, last_idx);

    TestEntrySubmitter entry_submitter(ledger2, last_idx);
    entry_submitter.write(true);
    // On restore, we write at the end of the last file is that file is not
    // complete
    REQUIRE(number_of_files_in_ledger_dir() == number_of_ledger_files);
  }

  SUBCASE("Restoring some committed chunks")
  {
    // This is the scenario on recovery
    size_t committed_idx = 0;
    INFO("Initialise first ledger with committed chunks");
    {
      Ledger ledger(ledger_dir, wf, chunk_threshold);
      TestEntrySubmitter entry_submitter(ledger);

      end_of_first_chunk_idx =
        initialise_ledger(entry_submitter, chunk_threshold, chunk_count);

      committed_idx = 2 * end_of_first_chunk_idx + 1;
      entry_submitter.write(true);
      last_idx = entry_submitter.get_last_idx();
      ledger.commit(committed_idx);
    }

    Ledger ledger2(ledger_dir, wf, chunk_threshold);
    read_entries_range_from_ledger(ledger2, 1, last_idx);

    // Restored ledger cannot be truncated before last idx of last committed
    // chunk
    TestEntrySubmitter entry_submitter(ledger2, last_idx);
    entry_submitter.truncate(committed_idx - 1); // Successful

    ledger2.truncate(committed_idx - 2); // Unsuccessful
    read_entries_range_from_ledger(ledger2, 1, end_of_first_chunk_idx);
  }

  SUBCASE("Restoring ledger with different chunking threshold")
  {
    INFO("Initialise first ledger with committed chunks");
    {
      Ledger ledger(ledger_dir, wf, chunk_threshold);
      TestEntrySubmitter entry_submitter(ledger);

      end_of_first_chunk_idx =
        initialise_ledger(entry_submitter, chunk_threshold, chunk_count);

      entry_submitter.write(true);
      last_idx = entry_submitter.get_last_idx();
    }

    INFO("Restore new ledger with twice the chunking threshold");
    {
      Ledger ledger2(ledger_dir, wf, 2 * chunk_threshold);
      read_entries_range_from_ledger(ledger2, 1, last_idx);

      TestEntrySubmitter entry_submitter(ledger2, last_idx);

      size_t orig_number_files = number_of_files_in_ledger_dir();
      while (number_of_files_in_ledger_dir() == orig_number_files)
      {
        entry_submitter.write(true);
      }
      last_idx = entry_submitter.get_last_idx();
    }

    INFO("Restore new ledger with half the chunking threshold");
    {
      Ledger ledger2(ledger_dir, wf, chunk_threshold / 2);
      read_entries_range_from_ledger(ledger2, 1, last_idx);

      TestEntrySubmitter entry_submitter(ledger2, last_idx);

      size_t orig_number_files = number_of_files_in_ledger_dir();
      while (number_of_files_in_ledger_dir() == orig_number_files)
      {
        entry_submitter.write(true);
      }
    }
  }
}

size_t number_open_fd()
{
  size_t fd_count = 0;
  for (auto const& f : fs::directory_iterator("/proc/self/fd"))
  {
    fd_count++;
  }
  return fd_count;
}

TEST_CASE("Limit number of open files")
{
  fs::remove_all(ledger_dir);

  size_t chunk_threshold = 30;
  size_t chunk_count = 5;
  size_t max_read_cache_size = 2;
  Ledger ledger(ledger_dir, wf, chunk_threshold, max_read_cache_size);
  TestEntrySubmitter entry_submitter(ledger);

  size_t initial_number_fd = number_open_fd();
  size_t last_idx = 0;

  size_t end_of_first_chunk_idx =
    initialise_ledger(entry_submitter, chunk_threshold, chunk_count);
  REQUIRE(number_open_fd() == initial_number_fd + chunk_count);

  INFO("Writing a new chunk opens a new file");
  {
    entry_submitter.write(true);
    last_idx = entry_submitter.get_last_idx();
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count + 1);
  }

  INFO("Commit closes files and reading committed chunks opens those");
  {
    ledger.commit(1); // No file committed
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count + 1);

    ledger.commit(end_of_first_chunk_idx); // One file now committed
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count);
    read_entry_from_ledger(ledger, 1);
    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx);
    // Committed file is open in read cache
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count + 1);

    ledger.commit(2 * end_of_first_chunk_idx); // Two files now committed
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count);
    read_entries_range_from_ledger(ledger, 1, 2 * end_of_first_chunk_idx);
    // Two committed files open in read cache
    REQUIRE(number_open_fd() == initial_number_fd + chunk_count + 1);

    ledger.commit(last_idx); // All but one file committed
    // One file open for write, two files open for read
    REQUIRE(number_open_fd() == initial_number_fd + 3);

    read_entries_range_from_ledger(ledger, 1, last_idx);
    // Number of open files is capped by size of read cache
    REQUIRE(number_open_fd() == initial_number_fd + 1 + max_read_cache_size);

    // Reading out of order succeeds
    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx);
    read_entries_range_from_ledger(
      ledger, 2 * end_of_first_chunk_idx, 3 * end_of_first_chunk_idx);
    read_entries_range_from_ledger(ledger, 1, last_idx);
    read_entries_range_from_ledger(
      ledger, 3 * end_of_first_chunk_idx, last_idx - 1);
    read_entries_range_from_ledger(ledger, 1, end_of_first_chunk_idx);
  }

  INFO("Close and commit latest file");
  {
    entry_submitter.write(true);
    entry_submitter.write(true);
    last_idx = entry_submitter.get_last_idx();
    ledger.commit(last_idx);

    read_entries_range_from_ledger(ledger, 1, last_idx);
    REQUIRE(number_open_fd() == initial_number_fd + max_read_cache_size);
  }

  INFO("Still possible to recover a new ledger");
  {
    initial_number_fd = number_open_fd();
    Ledger ledger2(ledger_dir, wf, chunk_threshold, max_read_cache_size);

    // Committed files are not open for write
    REQUIRE(number_open_fd() == initial_number_fd);

    read_entries_range_from_ledger(ledger2, 1, last_idx);
    REQUIRE(number_open_fd() == initial_number_fd + max_read_cache_size);
  }
}

TEST_CASE("Multiple ledger paths")
{
  static constexpr auto ledger_dir_2 = "ledger_dir_2";
  static constexpr auto empty_write_ledger_dir = "ledger_dir_empty";

  fs::remove_all(ledger_dir);
  fs::remove_all(ledger_dir_2);
  fs::remove_all(empty_write_ledger_dir);

  size_t max_read_cache_size = 2;
  size_t chunk_threshold = 30;
  size_t chunk_count = 5;

  size_t last_committed_idx = 0;
  size_t last_idx = 0;

  INFO("Write many entries on first ledger");
  {
    Ledger ledger(ledger_dir, wf, chunk_threshold);
    TestEntrySubmitter entry_submitter(ledger);

    // Writing some committed chunks...
    initialise_ledger(entry_submitter, chunk_threshold, chunk_count);
    last_committed_idx = entry_submitter.get_last_idx();
    ledger.commit(last_committed_idx);

    // ... and an uncommitted suffix
    bool is_committable = true;
    entry_submitter.write(is_committable);
    entry_submitter.write(is_committable);
    last_idx = entry_submitter.get_last_idx();
  }

  INFO("Copy uncommitted suffix from initial ledger directory");
  {
    fs::create_directory(ledger_dir_2);
    for (auto const& f : fs::directory_iterator(ledger_dir))
    {
      if (!is_ledger_file_committed(f.path().filename()))
      {
        fs::copy(f.path(), ledger_dir_2);
      }
    }
  }

  INFO("Restored ledger cannot read past uncommitted files");
  {
    Ledger ledger(ledger_dir_2, wf, chunk_threshold);

    for (size_t i = 1; i <= last_committed_idx; i++)
    {
      REQUIRE_FALSE(ledger.read_entry(i).has_value());
    }

    read_entry_from_ledger(ledger, last_idx);
  }

  INFO("Restore ledger with previous directory");
  {
    Ledger ledger(
      ledger_dir_2, wf, chunk_threshold, max_read_cache_size, {ledger_dir});

    for (size_t i = 1; i <= last_committed_idx; i++)
    {
      read_entry_from_ledger(ledger, i);
    }

    // Read framed entries across both directories
    read_entries_range_from_ledger(ledger, 1, last_idx);
  }

  INFO("Only committed files can be read from read-only directory");
  {
    Ledger ledger(
      empty_write_ledger_dir,
      wf,
      chunk_threshold,
      max_read_cache_size,
      {ledger_dir});

    for (size_t i = 1; i <= last_committed_idx; i++)
    {
      read_entry_from_ledger(ledger, i);
    }

    // Even though the ledger file for last_idx is in ledger_dir, the entry
    // cannot be read
    REQUIRE_FALSE(ledger.read_entry(last_idx).has_value());
  }
}