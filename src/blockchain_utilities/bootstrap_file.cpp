// Copyright (c) 2014-2024, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// LOG: Add std::unique_ptr, modularity and error handling

#include "bootstrap_serialization.h"
#include "serialization/binary_utils.h" // dump_binary(), parse_binary()
#include "bootstrap_file.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "bcutil"

namespace po = boost::program_options;
using namespace cryptonote;
using namespace epee;

namespace {
  const uint32_t blockchain_raw_magic = 0x28721586;
  const uint32_t header_size = 1024;
  const std::string refresh_string = "\r                                    \r";
}

bool BootstrapFile::open_writer(const boost::filesystem::path& file_path, uint64_t start_block, uint64_t stop_block) {
  const boost::filesystem::path dir_path = file_path.parent_path();
  if (!dir_path.empty()) {
    if (boost::filesystem::exists(dir_path) && !boost::filesystem::is_directory(dir_path)) {
      MFATAL("Export directory path is a file: " << dir_path);
      return false;
    }
    if (!boost::filesystem::exists(dir_path) && !boost::filesystem::create_directory(dir_path)) {
      MFATAL("Failed to create directory " << dir_path);
      return false;
    }
  }

  std::unique_ptr<std::ofstream> raw_data_file(new std::ofstream());
  m_raw_data_file = std::move(raw_data_file);

  bool initialize_file = !boost::filesystem::exists(file_path);
  uint64_t num_blocks = 0, block_first = 0;

  if (initialize_file) {
    MDEBUG("Creating new file");
  } else {
    std::streampos dummy_pos;
    uint64_t dummy_height = 0;
    num_blocks = count_blocks(file_path.string(), dummy_pos, dummy_height, block_first);
    MDEBUG("Appending to existing file with height: " << num_blocks + block_first - 1);
  }
  m_height = num_blocks + block_first;

  m_raw_data_file->open(file_path.string(), std::ios_base::binary | (initialize_file ? std::ios::trunc : std::ios::app));

  if (m_raw_data_file->fail()) return false;

  m_output_stream = std::make_unique<boost::iostreams::stream<boost::iostreams::back_insert_device<buffer_type>>>(m_buffer);

  if (initialize_file) initialize_file_content(start_block, stop_block);

  return true;
}

void BootstrapFile::initialize_file_content(uint64_t first_block, uint64_t last_block) {
  const uint32_t file_magic = blockchain_raw_magic;
  write_binary(file_magic);

  bootstrap::file_info bfi = {1, 0, header_size};
  bootstrap::blocks_info bbi = {first_block, last_block, 0};

  write_serialized_object(bfi);
  write_serialized_object(bbi);

  std::string padding(header_size - m_buffer.size(), 0);
  m_output_stream->write(padding.data(), padding.size());

  std::copy(m_buffer.begin(), m_buffer.end(), std::ostreambuf_iterator<char>(*m_raw_data_file));
}

void BootstrapFile::write_serialized_object(const auto& object) {
  blobdata bd = t_serializable_object_to_blob(object);
  uint32_t size = bd.size();
  write_binary(size);
  m_output_stream->write(bd.data(), bd.size());
}

void BootstrapFile::write_binary(uint32_t value) {
  std::string blob;
  if (!::serialization::dump_binary(value, blob)) {
    throw std::runtime_error("Error in serialization");
  }
  *m_raw_data_file << blob;
}

void BootstrapFile::flush_chunk() {
  m_output_stream->flush();

  uint32_t chunk_size = m_buffer.size();
  if (chunk_size > BUFFER_SIZE) {
    MWARNING("Chunk size " << chunk_size << " exceeds BUFFER_SIZE");
  }

  write_binary(chunk_size);

  if (m_max_chunk < chunk_size) m_max_chunk = chunk_size;

  long pos_before = m_raw_data_file->tellp();
  std::copy(m_buffer.begin(), m_buffer.end(), std::ostreambuf_iterator<char>(*m_raw_data_file));
  m_raw_data_file->flush();

  long num_chars_written = m_raw_data_file->tellp() - pos_before;
  if (static_cast<unsigned long>(num_chars_written) != chunk_size) {
    MFATAL("Error writing chunk at height: " << m_cur_height);
    throw std::runtime_error("Error writing chunk");
  }

  m_buffer.clear();
  reset_output_stream();
  MDEBUG("Flushed chunk: chunk_size: " << chunk_size);
}

void BootstrapFile::reset_output_stream() {
  m_output_stream = std::make_unique<boost::iostreams::stream<boost::iostreams::back_insert_device<buffer_type>>>(m_buffer);
}

void BootstrapFile::write_block(block& blk) {
  bootstrap::block_package bp;
  bp.block = blk;

  uint64_t block_height = boost::get<txin_gen>(blk.miner_tx.vin.front()).height;
  bp.txs = fetch_transactions(blk.tx_hashes);

  if (include_extra_block_data) {
    bp.block_weight = m_blockchain_storage->get_db().get_block_weight(block_height);
    bp.cumulative_difficulty = m_blockchain_storage->get_db().get_block_cumulative_difficulty(block_height);
    bp.coins_generated = m_blockchain_storage->get_db().get_block_already_generated_coins(block_height);
  }

  blobdata bd = t_serializable_object_to_blob(bp);
  m_output_stream->write(bd.data(), bd.size());
}

std::vector<transaction> BootstrapFile::fetch_transactions(const std::vector<crypto::hash>& tx_hashes) {
  std::vector<transaction> txs;
  for (const auto& tx_id : tx_hashes) {
    if (tx_id == crypto::null_hash) {
      throw std::runtime_error("Transaction ID is null");
    }
    txs.push_back(m_blockchain_storage->get_db().get_tx(tx_id));
  }
  return txs;
}

bool BootstrapFile::close() {
  if (m_raw_data_file->fail()) return false;
  m_raw_data_file->flush();
  return true;
}

bool BootstrapFile::store_blockchain_raw(Blockchain* blockchain_storage, tx_memory_pool* tx_pool, boost::filesystem::path& output_file, uint64_t start_block, uint64_t requested_block_stop) {
  m_blockchain_storage = blockchain_storage;
  m_tx_pool = tx_pool;
  uint64_t block_stop = determine_block_stop(requested_block_stop);
  if (!open_writer(output_file, start_block, block_stop)) return false;

  MINFO("Storing blocks raw data...");
  for (m_cur_height = std::max(start_block, m_height); m_cur_height <= block_stop; ++m_cur_height) {
    crypto::hash hash = m_blockchain_storage->get_block_id_by_height(m_cur_height);
    block blk;
    m_blockchain_storage->get_block_by_hash(hash, blk);
    write_block(blk);
    if (m_cur_height % NUM_BLOCKS_PER_CHUNK == 0) flush_chunk();
    display_progress(block_stop);
  }

  if (m_cur_height % NUM_BLOCKS_PER_CHUNK != 0) flush_chunk();
  MINFO("Largest chunk: " << m_max_chunk << " bytes");

  return close();
}

uint64_t BootstrapFile::determine_block_stop(uint64_t requested_block_stop) const {
  uint64_t block_stop = m_blockchain_storage->get_current_blockchain_height() - 1;
  if (requested_block_stop > 0 && requested_block_stop < block_stop) {
    block_stop = requested_block_stop;
    MINFO("Using requested block height: " << requested_block_stop);
  }
  return block_stop;
}

void BootstrapFile::display_progress(uint64_t block_stop) const {
  if (m_cur_height % 100 == 0) {
    std::cout << refresh_string << "block " << m_cur_height << "/" << block_stop << "\r" << std::flush;
  }
}
