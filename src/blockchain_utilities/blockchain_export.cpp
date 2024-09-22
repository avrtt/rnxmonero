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

#include "bootstrap_file.h"
#include "blocksdat_file.h"
#include "common/command_line.h"
#include "cryptonote_core/cryptonote_core.h"
#include "blockchain_db/blockchain_db.h"
#include "version.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "bcutil"

namespace po = boost::program_options;
using namespace cryptonote;
using namespace epee;

int main(int argc, char* argv[]) {
    TRY_ENTRY();

    epee::string_tools::set_module_name_and_folder(argv[0]);

    // Initialize log level and block boundaries
    uint32_t log_level = 0;
    uint64_t block_start = 0;
    uint64_t block_stop = 0;
    bool blocks_dat = false;

    tools::on_startup();
    boost::filesystem::path output_file_path;

    // Define command-line options
    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");
    
    const auto arg_output_file = command_line::arg_descriptor<std::string>{"output-file", "Specify output file", "", true};
    const auto arg_log_level = command_line::arg_descriptor<std::string>{"log-level", "0-4 or categories", ""};
    const auto arg_block_start = command_line::arg_descriptor<uint64_t>{"block-start", "Start at block number", block_start};
    const auto arg_block_stop = command_line::arg_descriptor<uint64_t>{"block-stop", "Stop at block number", block_stop};
    const auto arg_blocks_dat = command_line::arg_descriptor<bool>{"blocksdat", "Output in blocks.dat format", blocks_dat};

    // Add command line options
    command_line::add_arg(desc_cmd_sett, cryptonote::arg_data_dir);
    command_line::add_arg(desc_cmd_sett, arg_output_file);
    command_line::add_arg(desc_cmd_sett, cryptonote::arg_testnet_on);
    command_line::add_arg(desc_cmd_sett, cryptonote::arg_stagenet_on);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_block_start);
    command_line::add_arg(desc_cmd_sett, arg_block_stop);
    command_line::add_arg(desc_cmd_sett, arg_blocks_dat);
    command_line::add_arg(desc_cmd_only, command_line::arg_help);

    // Combine the options into a single description
    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett);

    // Parse command-line arguments
    po::variables_map vm;
    bool r = command_line::handle_error_helper(desc_options, [&]() {
        po::store(po::parse_command_line(argc, argv, desc_options), vm);
        po::notify(vm);
        return true;
    });
    if (!r) return 1;

    // Handle help argument
    if (command_line::get_arg(vm, command_line::arg_help)) {
        std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << std::endl << std::endl;
        std::cout << desc_options << std::endl;
        return 1;
    }

    // Configure logging
    mlog_configure(mlog_get_default_log_path("monero-blockchain-export.log"), true);
    const auto log_arg = command_line::get_arg(vm, arg_log_level);
    mlog_set_log(!command_line::is_arg_defaulted(vm, arg_log_level) ? log_arg.c_str() : (std::to_string(log_level) + ",bcutil:INFO").c_str());

    // Retrieve block start and stop arguments
    block_start = command_line::get_arg(vm, arg_block_start);
    block_stop = command_line::get_arg(vm, arg_block_stop);

    LOG_PRINT_L0("Starting...");

    // Handle testnet and stagenet options
    bool opt_testnet = command_line::get_arg(vm, cryptonote::arg_testnet_on);
    bool opt_stagenet = command_line::get_arg(vm, cryptonote::arg_stagenet_on);
    if (opt_testnet && opt_stagenet) {
        std::cerr << "Can't specify more than one of --testnet and --stagenet" << std::endl;
        return 1;
    }

    // Determine output format and file path
    bool opt_blocks_dat = command_line::get_arg(vm, arg_blocks_dat);
    std::string config_folder = command_line::get_arg(vm, cryptonote::arg_data_dir);
    
    output_file_path = command_line::has_arg(vm, arg_output_file)
                        ? boost::filesystem::path(command_line::get_arg(vm, arg_output_file))
                        : boost::filesystem::path(config_folder) / "export" / BLOCKCHAIN_RAW;

    LOG_PRINT_L0("Export output file: " << output_file_path.string());

    // Initialize blockchain storage
    LOG_PRINT_L0("Initializing source blockchain (BlockchainDB)");
    std::unique_ptr<BlockchainAndPool> core_storage = std::make_unique<BlockchainAndPool>();

    BlockchainDB* db = new_db();
    if (!db) {
        LOG_ERROR("Failed to initialize a database");
        throw std::runtime_error("Failed to initialize a database");
    }

    LOG_PRINT_L0("database: LMDB");

    // Open the database
    boost::filesystem::path folder(config_folder);
    folder /= db->get_db_name();
    const std::string filename = folder.string();

    LOG_PRINT_L0("Loading blockchain from folder " << filename << " ...");
    try {
        db->open(filename, DBF_RDONLY);
    } catch (const std::exception& e) {
        LOG_PRINT_L0("Error opening database: " << e.what());
        return 1;
    }

    // Initialize blockchain
    r = core_storage->blockchain.init(db, opt_testnet ? cryptonote::TESTNET : opt_stagenet ? cryptonote::STAGENET : cryptonote::MAINNET);
    if (!r) {
        LOG_ERROR("Failed to initialize source blockchain storage");
        return 1;
    }

    // Check if blockchain is pruned
    if (core_storage->blockchain.get_blockchain_pruning_seed() && !opt_blocks_dat) {
        LOG_PRINT_L0("Blockchain is pruned, cannot export");
        return 1;
    }

    LOG_PRINT_L0("Source blockchain storage initialized OK");
    LOG_PRINT_L0("Exporting blockchain raw data...");

    // Export blockchain data
    if (opt_blocks_dat) {
        BlocksdatFile blocksdat;
        r = blocksdat.store_blockchain_raw(&core_storage->blockchain, nullptr, output_file_path, block_stop);
    } else {
        BootstrapFile bootstrap;
        r = bootstrap.store_blockchain_raw(&core_storage->blockchain, nullptr, output_file_path, block_start, block_stop);
    }

    if (!r) {
        LOG_ERROR("Failed to export blockchain raw data");
        return 1;
    }

    LOG_PRINT_L0("Blockchain raw data exported OK");
    return 0;

    CATCH_ENTRY("Export error", 1);
}

