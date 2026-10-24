
#include "qso.h"

#define DEFAULT_SNR 100

extern "C" {
    #include "../qth/qth.h"
    #include "string.h"
    #include "utils.h"
}

static std::string make_answer_text(ftx_msg_type_t last_rx_type, std::string remote_callsign,
                                    std::string local_callsign, const int local_snr, std::string grid,
                                    bool local_std);

std::vector<std::string> split_text(std::string text) {
    std::vector<std::string> tokens;

    const char  delim = ' ';
    size_t      initialPos = 0;
    size_t      pos;
    std::string token;
    do {
        pos = text.find(delim, initialPos);
        token = text.substr(initialPos, pos - initialPos);
        if (!token.empty())
            tokens.push_back(token);
        initialPos = pos + 1;
    } while (pos != std::string::npos);
    return tokens;
}

Candidate::Candidate(std::string remote_callsign) {
    _remote_callsign = remote_callsign;
    _sent_snr = DEFAULT_SNR;
    _rcvd_snr = DEFAULT_SNR;
}

void Candidate::set_grid(std::string grid) {
    _grid = grid;
}

void Candidate::set_report(int report) {
    _rcvd_snr = report;
}

void Candidate::set_msg_type(ftx_msg_type_t msg_type) {
    _last_rx_type = msg_type;
}

void Candidate::set_local_snr(int snr) {
    _local_snr = snr;
}

void Candidate::set_rcvd_snr(int snr) {
    _rcvd_snr = snr;
}

bool Candidate::match_callsign(std::string callsign) {
    return callsign == _remote_callsign;
}

bool Candidate::is_finished() {
    return (_last_rx_type == FTX_MSG_TYPE_73) || (_last_rx_type == FTX_MSG_TYPE_RR73);
}

bool Candidate::can_be_saved() {
    return (!_remote_callsign.empty()) && (_rcvd_snr != DEFAULT_SNR) && (_sent_snr != DEFAULT_SNR) && !_saved;
}

std::string Candidate::get_tx_text(std::string local_callsign, std::string local_qth, bool local_std) {
    std::string answer =
        make_answer_text(_last_rx_type, _remote_callsign, local_callsign, _local_snr, local_qth, local_std);
    if ((_last_rx_type == FTX_MSG_TYPE_GRID) || (_last_rx_type == FTX_MSG_TYPE_REPORT)) {
        _sent_snr = _local_snr;
    }
    return answer;
}

void Candidate::save_qso(save_qso_cb_t save_qso_cb) {
    if (save_qso_cb && can_be_saved()) {
        save_qso_cb(_remote_callsign.c_str(), _grid.c_str(), _rcvd_snr, _sent_snr);
        _saved = true;
    }
}

FTxQsoProcessor::FTxQsoProcessor(std::string local_callsign, std::string local_qth, save_qso_cb_t save_qso_cb, int max_repeats) {
    _local_callsign = local_callsign;
    if (local_qth.size() > 4) {
        _local_qth = local_qth.substr(0, 4);
    } else {
        _local_qth = local_qth;
    }
    _save_qso_cb = save_qso_cb;
    _max_repeats = max_repeats;
}

FTxQsoProcessor::~FTxQsoProcessor() {
    if (_cur_candidate != NULL)
        delete _cur_candidate;
    if (_next_candidate != NULL)
        delete _next_candidate;
}

void FTxQsoProcessor::add_rx_text(std::string text, const int snr, ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    meta->type = FXT_MSG_TYPE_OTHER;
    meta->local_snr = snr;
    meta->to_me = false;
    meta->grid[0] = '\0';

    std::vector<std::string> tokens = split_text(text);

    if (tokens.empty()) {
        return;
    }

    if ((tokens.size() >= 5) && (text.find(';') != text.npos)) {
        // "A2AA RR73; R2RFE <RP79AA> +05"
        std::vector<std::string> new_tokens;
        if (tokens[0] == _local_callsign) {
            new_tokens.push_back(tokens[0]);
            new_tokens.push_back(tokens[3]);
            new_tokens.push_back(tokens[1].substr(0, tokens[1].size() - 1));
        } else {
            new_tokens.push_back(tokens[2]);
            new_tokens.push_back(tokens[3]);
            new_tokens.push_back(tokens[4]);
        }
        tokens = new_tokens;
    }

    for (auto &token : tokens) {
        if (!token.empty() && token[0] == '<') {
            token = token.substr(1, token.length() - 2);
        }
    }

    if (tokens[0] == "CQ") {
        process_cq(meta, tokens, snr);
    } else if (tokens.size() >= 3) {
        if (tokens[2] == "73") {
            process_73(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2] == "RRR") || (tokens[2] == "RR73")) {
            process_rr73(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2][0] == 'R') && ((tokens[2][1] == '+') || (tokens[2][1] == '-'))) {
            process_r_report(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2][0] == '+') || (tokens[2][0] == '-')) {
            process_report(meta, tokens, snr, *tx_msg);
        } else if (qth_grid_check(tokens[2].c_str())) {
            process_grid(meta, tokens, snr, *tx_msg);
        }
    }
}

void FTxQsoProcessor::process_grid(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                   ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto grid = tokens[2];
    meta->type = FTX_MSG_TYPE_GRID;
    strcpy(meta->grid, grid.c_str());
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        auto candidate_to_update = get_candidate_to_update(call_de);
        if (candidate_to_update != NULL) {
            (*candidate_to_update)->set_msg_type(meta->type);
            (*candidate_to_update)->set_local_snr(snr);
            (*candidate_to_update)->set_grid(grid);
            if ((*candidate_to_update == _cur_candidate) && _auto) {
                tx_msg.repeats = _max_repeats;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_report(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                     ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto rcvd_snr = std::stoi(tokens[2]);
    meta->type = FTX_MSG_TYPE_REPORT;
    meta->remote_snr = rcvd_snr;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        auto candidate_to_update = get_candidate_to_update(call_de);
        if (candidate_to_update != NULL) {
            (*candidate_to_update)->set_msg_type(meta->type);
            (*candidate_to_update)->set_local_snr(snr);
            (*candidate_to_update)->set_rcvd_snr(rcvd_snr);
            if ((*candidate_to_update == _cur_candidate) && _auto) {
                tx_msg.repeats = _max_repeats;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_r_report(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                       ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto rcvd_snr = std::stoi(tokens[2].substr(1, tokens[2].npos));
    meta->type = FTX_MSG_TYPE_R_REPORT;
    meta->remote_snr = rcvd_snr;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if ((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) {
            _cur_candidate->set_msg_type(meta->type);
            _cur_candidate->set_local_snr(snr);
            _cur_candidate->set_rcvd_snr(rcvd_snr);
            _cur_candidate->save_qso(_save_qso_cb);
            if (_auto) {
                tx_msg.repeats = 1;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_rr73(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                   ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    meta->type = FTX_MSG_TYPE_RR73;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if ((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) {
            _cur_candidate->set_msg_type(meta->type);
            _cur_candidate->set_local_snr(snr);
            _cur_candidate->save_qso(_save_qso_cb);
            if (_auto) {
                tx_msg.repeats = 1;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_73(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                 ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    meta->type = FTX_MSG_TYPE_73;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if ((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) {
            delete _cur_candidate;
            if (_next_candidate != NULL) {
                _cur_candidate = _next_candidate;
                _next_candidate = NULL;
                if (_auto) {
                    tx_msg.repeats = 1;
                    strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
                }
            } else {
                _cur_candidate = NULL;
            }
        }
    }
}

void FTxQsoProcessor::process_cq(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr) {
    meta->type = FTX_MSG_TYPE_CQ;
    size_t call_de_pos;
    if (is_cq_modifier(tokens[1].c_str()) && tokens.size() > 2) {
        call_de_pos = 2;
    } else {
        call_de_pos = 1;
    }
    strcpy(meta->call_de, tokens[call_de_pos].c_str());
    if (tokens.size() > call_de_pos + 1) {
        strcpy(meta->grid, tokens[call_de_pos + 1].c_str());
    }
}

void FTxQsoProcessor::set_auto(bool val) {
    _auto = val;
}

void FTxQsoProcessor::set_local_standard(bool val) {
    _local_std = val;
}

void FTxQsoProcessor::start_new_slot() {
    if (_next_candidate != NULL) {
        delete _next_candidate;
        _next_candidate = NULL;
    }
}

void FTxQsoProcessor::reset() {
    if (_cur_candidate != NULL) {
        delete _cur_candidate;
        _cur_candidate = NULL;
    }
    if (_next_candidate != NULL) {
        delete _next_candidate;
        _next_candidate = NULL;
    }
}

void FTxQsoProcessor::start_qso(ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    auto type = meta->type;
    if (!meta->to_me) {
        type = FTX_MSG_TYPE_CQ;
    } else if (meta->type == FTX_MSG_TYPE_73) {
        return;
    }
    get_or_create_cur_candidate(meta->call_de);
    _cur_candidate->set_local_snr(meta->local_snr);
    _cur_candidate->set_msg_type(type);

    tx_msg->repeats = _max_repeats;
    switch (type) {
    case FTX_MSG_TYPE_CQ:
    case FTX_MSG_TYPE_GRID:
        _cur_candidate->set_grid(meta->grid);
        break;
    case FTX_MSG_TYPE_R_REPORT:
        tx_msg->repeats = 1;
    case FTX_MSG_TYPE_REPORT:
        _cur_candidate->set_rcvd_snr(meta->remote_snr);
        break;
    case FTX_MSG_TYPE_RR73:
        tx_msg->repeats = 1;
        break;

    default:
        break;
    }
    strcpy(tx_msg->msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth, _local_std).c_str());
}

bool FTxQsoProcessor::can_save_qso() {
    return _cur_candidate && _cur_candidate->can_be_saved();
}

bool FTxQsoProcessor::force_save_qso() {
    if (_cur_candidate) {
        _cur_candidate->save_qso(_save_qso_cb);
        delete _cur_candidate;
        _cur_candidate = NULL;
        return true;
    } else {
        return false;
    }
}

Candidate **FTxQsoProcessor::get_candidate_to_update(std::string call_de) {
    if (_cur_candidate == NULL) {
        // Start new QSO
        _cur_candidate = new Candidate(call_de);
    }
    Candidate **candidate_to_update = NULL;
    if (_cur_candidate->match_callsign(call_de)) {
        candidate_to_update = &_cur_candidate;
    } else if (_next_candidate == NULL) {
        _next_candidate = new Candidate(call_de);
        candidate_to_update = &_next_candidate;
    }
    return candidate_to_update;
}

Candidate *FTxQsoProcessor::get_or_create_cur_candidate(std::string remote_callsign) {
    // Try to continue current QSO
    if ((_cur_candidate != NULL) && !_cur_candidate->match_callsign(remote_callsign)) {
        delete _cur_candidate;
        _cur_candidate = NULL;
    }
    if (_cur_candidate == NULL) {
        _cur_candidate = new Candidate(remote_callsign);
    }
    return _cur_candidate;
}

bool FTxQsoProcessor::has_current() {
    return _cur_candidate != NULL;
}

/*
 * With a non-standard own call (KF0QGT/6Y) the answers follow WSJT-X: the
 * messages without a report go as type 4, "<W1ABC> KF0QGT/6Y RR73", with
 * our call in plain text, so a station that has not heard us before learns
 * it. The reports go as type 1 with our call as a hash, "W1ABC <KF0QGT/6Y>
 * -05"; there is no grid.
 */
static std::string make_answer_text(ftx_msg_type_t last_rx_type, std::string remote_callsign,
                                    std::string local_callsign, const int local_snr, std::string grid,
                                    bool local_std) {
    char answer[35];
    int  len = 0;
    switch (last_rx_type) {
    case FTX_MSG_TYPE_CQ:
        if (!local_std) {
            len = snprintf(answer, 35, "<%s> %s", remote_callsign.c_str(), local_callsign.c_str());
            break;
        }
        len = snprintf(answer, 35, "%s %s %s", remote_callsign.c_str(), local_callsign.c_str(), grid.c_str());
        break;
    case FTX_MSG_TYPE_GRID:
        len = snprintf(answer, 35, "%s %s %+03d", remote_callsign.c_str(), local_callsign.c_str(), local_snr);
        break;
    case FTX_MSG_TYPE_REPORT:
        len = snprintf(answer, 35, "%s %s R%+03d", remote_callsign.c_str(), local_callsign.c_str(), local_snr);
        break;
    case FTX_MSG_TYPE_R_REPORT:
        len = snprintf(answer, 35, local_std ? "%s %s RR73" : "<%s> %s RR73", remote_callsign.c_str(),
                       local_callsign.c_str());
        break;
    case FTX_MSG_TYPE_RR73:
        len = snprintf(answer, 35, local_std ? "%s %s 73" : "<%s> %s 73", remote_callsign.c_str(),
                       local_callsign.c_str());
        break;
    }
    return std::string(answer, len);
}

FTxQsoProcessor *ftx_qso_processor_init(const char *local_callsign, const char *qth, save_qso_cb_t save_qso_cb,
                                        int max_repeats) {
    return new FTxQsoProcessor(local_callsign, qth, save_qso_cb, max_repeats);
}

void ftx_qso_processor_delete(FTxQsoProcessor *p) {
    delete p;
}

void ftx_qso_processor_set_auto(FTxQsoProcessor *p, bool val) {
    p->set_auto(val);
}

void ftx_qso_processor_set_local_standard(FTxQsoProcessor *p, bool val) {
    p->set_local_standard(val);
}

void ftx_qso_processor_start_new_slot(FTxQsoProcessor *p) {
    p->start_new_slot();
}

void ftx_qso_processor_reset(FTxQsoProcessor *p) {
    p->reset();
}

void ftx_qso_processor_add_rx_text(FTxQsoProcessor *p, const char *text, const int snr, ftx_msg_meta_t *meta,
                                   ftx_tx_msg_t *tx_msg) {
    p->add_rx_text(text, snr, meta, tx_msg);
}

void ftx_qso_processor_start_qso(FTxQsoProcessor *p, ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    p->start_qso(meta, tx_msg);
}

bool ftx_qso_processor_can_save_qso(FTxQsoProcessor *p) {
    return p->can_save_qso();
}

bool ftx_qso_processor_force_save_qso(FTxQsoProcessor *p) {
    return p->force_save_qso();
}

bool ftx_qso_processor_has_current(FTxQsoProcessor *p) {
    if (!p) return false;
    return p->has_current();
}
