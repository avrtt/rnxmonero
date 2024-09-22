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

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include "common/command_line.h"
#include "serialization/crypto.h"
#include "cryptonote_core/cryptonote_core.h"
#include "version.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "bcutil"

namespace po = boost::program_options;
using namespace epee;
using namespace cryptonote;

namespace {

constexpr uint64_t INVALID_AMOUNT = std::numeric_limits<uint64_t>::max();

// Helper to load outputs from a file
std::map<uint64_t, uint64_t> load_outputs_from_file(const std::string &filename) {
    std::map<uint64_t, uint64_t> outputs;
    FILE *f = fopen(filename.c_str(), "r");
    if (!f) {
        MERROR("Failed to load outputs from " << filename << ": " << strerror(errno));
        return {};
    }

    char line[256];
    uint64_t amount = INVALID_AMOUNT;

    while (fgets(line, sizeof(line), f)) {
        const size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (line[0] == '\0') continue;

        uint64_t offset, num_offsets;
        if (sscanf(line, "@%" PRIu64, &amount) == 1) {
            continue;
        } else if (amount == INVALID_AMOUNT) {
            MERROR("Bad format in " << filename);
            continue;
        }

        if (sscanf(line, "%" PRIu64 "*%" PRIu64, &offset, &num_offsets) == 2 && num_offsets < INVALID_AMOUNT - offset) {
            outputs[amount] += num_offsets;
        } else if (sscanf(line, "%" PRIu64, &offset) == 1) {
            outputs[amount] += 1;
        } else {
            MERROR("Bad format in " << filename);
        }
    }

    fclose(f);
    return outputs;
}

// Helper to handle command line arguments
bool handle_command_line(int argc, char* argv[], po::variables_map &vm, const po::options_description &desc_options) {
    return command_line::handle_error_helper(desc_options, [&]() {
        po::store(po::command_line_parser(argc, argv).options(desc_options).run(), vm);
        po::notify(vm);
        return true;
    });
}

// Configure logging
void configure_logging(const po::variables_map &vm, const command_line::arg_descriptor<std::string> &arg_log_level, uint32_t log_level) {
    mlog_configure(mlog_get_default_log_path("monero-blockchain-prune-known-spent-data.log"), true);
    if (!command_line::is_arg_defaulted(vm, arg_log_level)) {
        mlog_set_log(command_line::get_arg(vm, arg_log_level).c_str());
    } else {
        mlog_set_log(std::to_string(log_level) + ",bcutil:INFO");
    }
}

// Load blockchain database
BlockchainDB* load_blockchain_db(const std::string &data_dir, network_type net_type) {
    std::unique_ptr<BlockchainAndPool> core_storage = std::make_unique<BlockchainAndPool>();
    BlockchainDB *db = new_db();
    if (!db) {
        LOG_ERROR("Failed to initialize a database");
        throw std::runtime_error("Failed to initialize a database");
    }

    const std::string db_path = (boost::filesystem::path(data_dir) / db->get_db_name()).string();
    LOG_PRINT_L0("Loading blockchain from folder " << db_path << " ...");

    try {
        db->open(db_path, 0);
    } catch (const std::exception &e) {
        LOG_PRINT_L0("Error opening database: " << e.what());
        delete db;
        return nullptr;
    }

    if (!core_storage->blockchain.init(db, net_type)) {
        LOG_ERROR("Failed to initialize source blockchain storage");
        delete db;
        return nullptr;
    }

    return db;
}

// Scan transactions for spent outputs
std::map<uint64_t, uint64_t> scan_for_spent_outputs(BlockchainDB* db) {
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> outputs;

    LOG_PRINT_L0("Scanning for known spent data...");
    db->for_all_transactions([&](const crypto::hash &txid, const cryptonote::transaction &tx) {
        const bool is_miner_tx = tx.vin.size() == 1 && tx.vin[0].type() == typeid(txin_gen);
        for (const auto &in : tx.vin) {
            if (in.type() != typeid(txin_to_key)) continue;
            const auto &txin = boost::get<txin_to_key>(in);
            if (txin.amount == 0) continue;
            outputs[txin.amount].second++;
        }

        for (const auto &out : tx.vout) {
            uint64_t amount = out.amount;
            if (is_miner_tx && tx.version >= 2) amount = 0;
            if (amount == 0 || out.target.type() != typeid(txout_to_key)) continue;
            outputs[amount].first++;
        }
        return true;
    }, true);

    std::map<uint64_t, uint64_t> known_spent_outputs;
    for (const auto &entry : outputs) {
        known_spent_outputs[entry.first] = entry.second.second;
    }

    return known_spent_outputs;
}

// Prune spent outputs from the blockchain
void prune_spent_outputs(BlockchainDB* db, const std::map<uint64_t, uint64_t>& known_spent_outputs, bool verbose, bool dry_run) {
    size_t num_total_outputs = 0, num_prunable_outputs = 0, num_known_spent_outputs = 0, num_eligible_outputs = 0, num_eligible_known_spent_outputs = 0;

    db->batch_start();
    for (const auto& [amount, spent_count] : known_spent_outputs) {
        uint64_t num_outputs = db->get_num_outputs(amount);
        num_total_outputs += num_outputs;
        num_known_spent_outputs += spent_count;

        if (amount == 0 || is_valid_decomposed_amount(amount)) {
            if (verbose) MINFO("Ignoring output value " << amount << ", with " << num_outputs << " outputs");
            continue;
        }

        num_eligible_outputs += num_outputs;
        num_eligible_known_spent_outputs += spent_count;

        if (verbose) MINFO(amount << ": " << spent_count << "/" << num_outputs);
        if (num_outputs > spent_count) continue;
        if (num_outputs < spent_count) {
            MERROR("More outputs are spent than known for amount " << amount << ", not touching");
            continue;
        }

        if (verbose) MINFO("Pruning data for " << num_outputs << " outputs");
        if (!dry_run) db->prune_outputs(amount);

        num_prunable_outputs += spent_count;
    }
    db->batch_stop();

    MINFO("Total outputs: " << num_total_outputs);
    MINFO("Known spent outputs: " << num_known_spent_outputs);
    MINFO("Eligible outputs: " << num_eligible_outputs);
    MINFO("Eligible known spent outputs: " << num_eligible_known_spent_outputs);
    MINFO("Prunable outputs: " << num_prunable_outputs);
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    TRY_ENTRY();

    epee::string_tools::set_module_name_and_folder(argv[0]);

    tools::on_startup();
    uint32_t log_level = 0;

    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");
    
    const command_line::arg_descriptor<std::string> arg_log_level = {"log-level", "0-4 or categories", ""};
    const command_line::arg_descriptor<bool> arg_verbose = {"verbose", "Verbose output", false};
    const command_line::arg_descriptor<bool> arg_dry_run = {"dry-run", "Do not actually prune", false};
    const command_line::arg_descriptor<std::string> arg_input = {"input", "Path to the known spent outputs file"};

    command_line::add_arg(desc_cmd_sett, cryptonote::arg_data_dir);
    command_line::add_arg(desc_cmd_sett, cryptonote::arg_testnet_on);
    command_line::add_arg(desc_cmd_sett, cryptonote::arg_stagenet_on);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_verbose);
    command_line::add_arg(desc_cmd_sett, arg_dry_run);
    command_line::add_arg(desc_cmd_sett, arg_input);
    command_line::add_arg(desc_cmd_only, command_line::arg_help);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett);

    po::variables_map vm;
    if (!handle_command_line(argc, argv, vm, desc_options)) return 1;

    if (command_line::get_arg(vm, command_line::arg_help)) {
        std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << std::endl;
        std::cout << desc_options << std::endl;
        return 1;
    }

    configure_logging(vm, arg_log_level, log_level);

    LOG_PRINT_L0("Starting...");

    std::string opt_data_dir = command_line::get_arg(vm, cryptonote::arg_data_dir);
    bool opt_testnet = command_line::get_arg(vm, cryptonote::arg_testnet_on);
    bool opt_stagenet = command_line::get_arg(vm, cryptonote::arg_stagenet_on);
    network_type net_type = opt_testnet ? TESTNET : opt_stagenet ? STAGENET : MAINNET;

    const bool opt_verbose = command_line::get_arg(vm, arg_verbose);
    const bool opt_dry_run = command_line::get_arg(vm, arg_dry_run);
    const std::string input = command_line::get_arg(vm, arg_input);

    BlockchainDB* db = load_blockchain_db(opt_data_dir, net_type);
    if (!db) return 1;

    std::map<uint64_t, uint64_t> known_spent_outputs = input.empty() ? scan_for_spent_outputs(db) : load_outputs_from_file(input);

    prune_spent_outputs(db, known_spent_outputs, opt_verbose, opt_dry_run);

    LOG_PRINT_L0("Blockchain known spent data pruned OK");
    return 0;

    CATCH_ENTRY("Error", 1);
}

