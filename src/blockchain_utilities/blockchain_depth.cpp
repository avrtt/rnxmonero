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

// Changelog: Add range-based loops, early returns, lambdas and std::any_of 

#include <boost/range/adaptor/transformed.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string.hpp>
#include "common/command_line.h"
#include "common/varint.h"
#include "cryptonote_core/cryptonote_core.h"
#include "version.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "bcutil"

namespace po = boost::program_options;
using namespace epee;
using namespace cryptonote;

int main(int argc, char* argv[])
{
  TRY_ENTRY();

  epee::string_tools::set_module_name_and_folder(argv[0]);
  tools::on_startup();

  uint32_t log_level = 0;
  boost::filesystem::path output_file_path;

  // Command line arguments definition
  po::options_description desc_cmd_only("Command line options");
  po::options_description desc_cmd_sett("Command line options and settings options");

  const command_line::arg_descriptor<std::string> arg_log_level{"log-level", "0-4 or categories", ""};
  const command_line::arg_descriptor<std::string> arg_txid{"txid", "Get min depth for this txid", ""};
  const command_line::arg_descriptor<uint64_t> arg_height{"height", "Get min depth for all txes at this height", 0};
  const command_line::arg_descriptor<bool> arg_include_coinbase{"include-coinbase", "Include coinbase in the average", false};

  command_line::add_arg(desc_cmd_sett, cryptonote::arg_data_dir);
  command_line::add_arg(desc_cmd_sett, cryptonote::arg_testnet_on);
  command_line::add_arg(desc_cmd_sett, cryptonote::arg_stagenet_on);
  command_line::add_arg(desc_cmd_sett, arg_log_level);
  command_line::add_arg(desc_cmd_sett, arg_txid);
  command_line::add_arg(desc_cmd_sett, arg_height);
  command_line::add_arg(desc_cmd_sett, arg_include_coinbase);
  command_line::add_arg(desc_cmd_only, command_line::arg_help);

  po::options_description desc_options("Allowed options");
  desc_options.add(desc_cmd_only).add(desc_cmd_sett);

  po::variables_map vm;
  bool parse_success = command_line::handle_error_helper(desc_options, [&]() {
    auto parser = po::command_line_parser(argc, argv).options(desc_options);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });

  if (!parse_success) return 1;

  if (command_line::get_arg(vm, command_line::arg_help)) {
    std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << std::endl;
    std::cout << desc_options << std::endl;
    return 1;
  }

  mlog_configure(mlog_get_default_log_path("monero-blockchain-depth.log"), true);

  if (!command_line::is_arg_defaulted(vm, arg_log_level))
    mlog_set_log(command_line::get_arg(vm, arg_log_level).c_str());
  else
    mlog_set_log((std::to_string(log_level) + ",bcutil:INFO").c_str());

  LOG_PRINT_L0("Starting...");

  // Get program options
  auto opt_data_dir = command_line::get_arg(vm, cryptonote::arg_data_dir);
  bool opt_testnet = command_line::get_arg(vm, cryptonote::arg_testnet_on);
  bool opt_stagenet = command_line::get_arg(vm, cryptonote::arg_stagenet_on);
  network_type net_type = opt_testnet ? TESTNET : (opt_stagenet ? STAGENET : MAINNET);

  auto opt_txid_string = command_line::get_arg(vm, arg_txid);
  uint64_t opt_height = command_line::get_arg(vm, arg_height);
  bool opt_include_coinbase = command_line::get_arg(vm, arg_include_coinbase);

  if (!opt_txid_string.empty() && opt_height) {
    std::cerr << "txid and height cannot be given at the same time" << std::endl;
    return 1;
  }

  crypto::hash opt_txid = crypto::null_hash;
  if (!opt_txid_string.empty() && !epee::string_tools::hex_to_pod(opt_txid_string, opt_txid)) {
    std::cerr << "Invalid txid" << std::endl;
    return 1;
  }

  // Blockchain initialization
  LOG_PRINT_L0("Initializing source blockchain (BlockchainDB)");

  auto core_storage = std::make_unique<BlockchainAndPool>();
  std::unique_ptr<BlockchainDB> db(new_db());

  if (!db) {
    LOG_ERROR("Failed to initialize a database");
    throw std::runtime_error("Failed to initialize a database");
  }

  LOG_PRINT_L0("database: LMDB");

  const std::string filename = (boost::filesystem::path(opt_data_dir) / db->get_db_name()).string();
  LOG_PRINT_L0("Loading blockchain from folder " << filename << " ...");

  try {
    db->open(filename, DBF_RDONLY);
  } catch (const std::exception& e) {
    LOG_PRINT_L0("Error opening database: " << e.what());
    return 1;
  }

  if (!core_storage->blockchain.init(db.get(), net_type)) {
    LOG_PRINT_L0("Failed to initialize source blockchain storage");
    return 1;
  }

  LOG_PRINT_L0("Source blockchain storage initialized OK");

  // Gather transactions to check
  std::vector<crypto::hash> start_txids;
  if (!opt_txid_string.empty()) {
    start_txids.push_back(opt_txid);
  } else {
    auto bd = db->get_block_blob_from_height(opt_height);
    cryptonote::block b;

    if (!cryptonote::parse_and_validate_block_from_blob(bd, b)) {
      LOG_PRINT_L0("Bad block from db");
      return 1;
    }

    for (const auto& txid : b.tx_hashes) start_txids.push_back(txid);
    if (opt_include_coinbase) start_txids.push_back(cryptonote::get_transaction_hash(b.miner_tx));
  }

  if (start_txids.empty()) {
    LOG_PRINT_L0("No transaction(s) to check");
    return 1;
  }

  // Calculate depth for transactions
  std::vector<uint64_t> depths;

  for (const auto& start_txid : start_txids) {
    uint64_t depth = 0;
    bool coinbase = false;
    std::vector<crypto::hash> txids{start_txid};

    while (!coinbase) {
      LOG_PRINT_L0("Considering " << txids.size() << " transaction(s) at depth " << depth);

      std::vector<crypto::hash> new_txids;

      for (const auto& txid : txids) {
        cryptonote::blobdata bd;

        if (!db->get_pruned_tx_blob(txid, bd)) {
          LOG_PRINT_L0("Failed to get txid " << txid << " from db");
          return 1;
        }

        cryptonote::transaction tx;

        if (!cryptonote::parse_and_validate_tx_base_from_blob(bd, tx)) {
          LOG_PRINT_L0("Bad tx: " << txid);
          return 1;
        }

        for (const auto& vin : tx.vin) {
          if (vin.type() == typeid(cryptonote::txin_gen)) {
            coinbase = true;
            break;
          } else if (vin.type() == typeid(cryptonote::txin_to_key)) {
            const auto& txin = boost::get<cryptonote::txin_to_key>(vin);
            const auto absolute_offsets = cryptonote::relative_output_offsets_to_absolute(txin.key_offsets);

            for (uint64_t offset : absolute_offsets) {
              auto od = db->get_output_key(txin.amount, offset);
              auto block_hash = db->get_block_hash_from_height(od.height);

              bd = db->get_block_blob(block_hash);
              cryptonote::block b;

              if (!cryptonote::parse_and_validate_block_from_blob(bd, b)) {
                LOG_PRINT_L0("Bad block from db");
                return 1;
              }

              auto match_txout_key = [&](const auto& txout, const crypto::hash& tx_hash) {
                if (boost::get<cryptonote::txout_to_key>(txout.target).key == od.pubkey) {
                  new_txids.push_back(tx_hash);
                  return true;
                }
                return false;
              };

              if (std::any_of(b.miner_tx.vout.begin(), b.miner_tx.vout.end(), [&](const auto& vout) {
                    return match_txout_key(vout, cryptonote::get_transaction_hash(b.miner_tx));
                  }))
                break;

              for (const auto& block_txid : b.tx_hashes) {
                if (!db->get_pruned_tx_blob(block_txid, bd)) {
                  LOG_PRINT_L0("Failed to get txid " << block_txid << " from db");
                  return 1;
                }

                cryptonote::transaction tx2;

                if (!cryptonote::parse_and_validate_tx_base_from_blob(bd, tx2)) {
                  LOG_PRINT_L0("Bad tx: " << block_txid);
                  return 1;
                }

                if (std::any_of(tx2.vout.begin(), tx2.vout.end(), [&](const auto& vout) {
                      return match_txout_key(vout, block_txid);
                    }))
                  break;
              }
            }
          } else {
            LOG_PRINT_L0("Bad vin type in txid " << txid);
            return 1;
          }
        }
      }

      if (!coinbase) {
        std::swap(txids, new_txids);
        ++depth;
      }
    }

    LOG_PRINT_L0("Min depth for txid " << start_txid << ": " << depth);
    depths.push_back(depth);
  }

  // Output depth results
  uint64_t cumulative_depth = std::accumulate(depths.begin(), depths.end(), 0ULL);
  LOG_PRINT_L0("Average min depth for " << start_txids.size() << " transaction(s): " << cumulative_depth / static_cast<float>(depths.size()));
  LOG_PRINT_L0("Median min depth for " << start_txids.size() << " transaction(s): " << epee::misc_utils::median(depths));

  core_storage->blockchain.deinit();
  return 0;

  CATCH_ENTRY("Depth query error", 1);
}

