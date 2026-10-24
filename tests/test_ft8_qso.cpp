#include "../src/ft8/qso.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>

#include <tuple>
#include <vector>

using Catch::Matchers::Equals;

std::vector<std::tuple<std::string, std::string, int, int>> qso_vec;

void save_qso(const char *remote_callsign, const char *remote_grid, const int r_snr, const int s_snr) {
    qso_vec.push_back(std::make_tuple(std::string(remote_callsign), std::string(remote_grid), r_snr, s_snr));
}

TEST_CASE("Split text", "[ft8_qso]") {
    auto tokens = split_text("CQ EA0DX KO12");
    REQUIRE(tokens.size() == 3);
    REQUIRE(tokens[0] == "CQ");
    REQUIRE(tokens[1] == "EA0DX");
    REQUIRE(tokens[2] == "KO12");
}

TEST_CASE("Split short text", "[ft8_qso]") {
    auto tokens = split_text("CQ EA0DX123");
    REQUIRE(tokens.size() == 2);
    REQUIRE(tokens[0] == "CQ");
    REQUIRE(tokens[1] == "EA0DX123");
}
TEST_CASE("Split text with extra spaces", "[ft8_qso]") {
    auto tokens = split_text(" CQ   EA0DX123 ");
    REQUIRE(tokens.size() == 2);
    REQUIRE(tokens[0] == "CQ");
    REQUIRE(tokens[1] == "EA0DX123");
}

TEST_CASE("Check fill meta", "[ft8_qso]") {
    const int      default_snr = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    ftx_msg_meta_t meta = {.grid = "empty",
                           .call_de = "empty call",
                           .local_snr = default_snr,
                           .remote_snr = default_snr,
                           .to_me = false,
                           .type = FXT_MSG_TYPE_OTHER};
    ftx_tx_msg_t   tx_msg;
    SECTION("cq") {
        meta.to_me = true;
        q.add_rx_text("CQ EA0DX KO12", 12, &meta, &tx_msg);
        REQUIRE_THAT(meta.call_de, Equals("EA0DX"));
        REQUIRE_THAT(meta.grid, Equals("KO12"));
        REQUIRE(meta.local_snr == 12);
        REQUIRE(meta.remote_snr == default_snr);
        REQUIRE(!meta.to_me);
        REQUIRE(meta.type == FTX_MSG_TYPE_CQ);
    }
    SECTION("cq without grid") {
        q.add_rx_text("CQ EA1000DX", 12, &meta, &tx_msg);
        REQUIRE_THAT(meta.call_de, Equals("EA1000DX"));
        REQUIRE_THAT(meta.grid, Equals(""));
        REQUIRE(meta.local_snr == 12);
        REQUIRE(meta.remote_snr == default_snr);
        REQUIRE(!meta.to_me);
        REQUIRE(meta.type == FTX_MSG_TYPE_CQ);
    }
    SECTION("cq with mod") {
        SECTION("CQ with mod1") {
            q.add_rx_text("CQ AS EA0DX KO12", 12, &meta, &tx_msg);
        }
        SECTION("CQ with mod2") {
            q.add_rx_text("CQ POTA EA0DX KO12", 12, &meta, &tx_msg);
        }
        SECTION("CQ with mod3") {
            q.add_rx_text("CQ 999 EA0DX KO12", 12, &meta, &tx_msg);
        }
        REQUIRE_THAT(meta.call_de, Equals("EA0DX"));
        REQUIRE_THAT(meta.grid, Equals("KO12"));
        REQUIRE(meta.local_snr == 12);
        REQUIRE(meta.remote_snr == default_snr);
        REQUIRE(!meta.to_me);
        REQUIRE(meta.type == FTX_MSG_TYPE_CQ);
    }
    SECTION("messages") {
        SECTION("grid message") {
            SECTION("not to me") {
                q.add_rx_text("PU2GK EA0DX KO12", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("to me") {
                q.add_rx_text("R2RFE EA0DX KO12", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            REQUIRE_THAT(meta.grid, Equals("KO12"));
            REQUIRE(meta.remote_snr == default_snr);
            REQUIRE(meta.type == FTX_MSG_TYPE_GRID);
        }
        SECTION("report message") {
            SECTION("not to me") {
                q.add_rx_text("PU2GK EA0DX +08", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("to me") {
                q.add_rx_text("R2RFE EA0DX +08", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            REQUIRE_THAT(meta.grid, Equals(""));
            REQUIRE(meta.remote_snr == 8);
            REQUIRE(meta.type == FTX_MSG_TYPE_REPORT);
        }
        SECTION("r report message") {
            SECTION("not to me") {
                q.add_rx_text("PU2GK EA0DX R+08", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("to me") {
                q.add_rx_text("R2RFE EA0DX R+08", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            REQUIRE_THAT(meta.grid, Equals(""));
            REQUIRE(meta.remote_snr == 8);
            REQUIRE(meta.type == FTX_MSG_TYPE_R_REPORT);
        }
        SECTION("RR message") {
            SECTION("RR73 not to me") {
                q.add_rx_text("PU2GK EA0DX RR73", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("RR73 to me") {
                q.add_rx_text("R2RFE EA0DX RR73", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            SECTION("RRR not to me") {
                q.add_rx_text("PU2GK EA0DX RRR", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("RRR to me") {
                q.add_rx_text("R2RFE EA0DX RRR", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            REQUIRE_THAT(meta.grid, Equals(""));
            REQUIRE(meta.remote_snr == default_snr);
            REQUIRE(meta.type == FTX_MSG_TYPE_RR73);
        }
        SECTION("73 message") {
            SECTION("not to me") {
                q.add_rx_text("PU2GK EA0DX 73", 12, &meta, &tx_msg);
                REQUIRE(!meta.to_me);
            }
            SECTION("to me") {
                q.add_rx_text("R2RFE EA0DX 73", 12, &meta, &tx_msg);
                REQUIRE(meta.to_me);
            }
            REQUIRE_THAT(meta.grid, Equals(""));
            REQUIRE(meta.remote_snr == default_snr);
            REQUIRE(meta.type == FTX_MSG_TYPE_73);
        }
        REQUIRE(meta.local_snr == 12);
        REQUIRE_THAT(meta.call_de, Equals("EA0DX"));
    }
    SECTION("DXpedition report") {
        meta.to_me = false;
        q.add_rx_text("A2AA RR73; R2RFE <RP79AA> +05", 12, &meta, &tx_msg);
        REQUIRE_THAT(meta.call_de, Equals("RP79AA"));
        REQUIRE_THAT(meta.grid, Equals(""));
        REQUIRE(meta.local_snr == 12);
        REQUIRE(meta.remote_snr == 5);
        REQUIRE(meta.to_me);
        REQUIRE(meta.type == FTX_MSG_TYPE_REPORT);
    }
    SECTION("DXpedition RR73") {
        meta.to_me = false;
        q.add_rx_text("R2RFE RR73; A2AA <RP79AA> +05", 12, &meta, &tx_msg);
        REQUIRE_THAT(meta.call_de, Equals("RP79AA"));
        REQUIRE_THAT(meta.grid, Equals(""));
        REQUIRE(meta.local_snr == 12);
        REQUIRE(meta.remote_snr == default_snr);
        REQUIRE(meta.to_me);
        REQUIRE(meta.type == FTX_MSG_TYPE_RR73);
    }
    qso_vec.clear();
}

TEST_CASE("Check populate tx_msg", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg = {.msg = "empty msg", .repeats = default_repeats};
    SECTION("No answer") {
        SECTION("CQ") {
            q.add_rx_text("CQ EA0DX KO12", 12, &meta, &tx_msg);
        }
        SECTION("R report without history") {
            q.add_rx_text("R2RFE EA0DX R+04", 12, &meta, &tx_msg);
        }
        SECTION("RR73 report without history") {
            q.add_rx_text("R2RFE EA0DX RR73", 12, &meta, &tx_msg);
        }
        SECTION("73") {
            q.add_rx_text("R2RFE EA0DX 73", 12, &meta, &tx_msg);
        }
        SECTION("GRID not to me") {
            q.add_rx_text("PU2GK EA0DX KS53", 12, &meta, &tx_msg);
        }
        SECTION("REPORT not to me") {
            q.add_rx_text("PU2GK EA0DX +20", 12, &meta, &tx_msg);
        }
        SECTION("R_REPORT not to me") {
            q.add_rx_text("PU2GK EA0DX R+00", 12, &meta, &tx_msg);
        }
        SECTION("RR73 not to me") {
            q.add_rx_text("PU2GK EA0DX RR73", 12, &meta, &tx_msg);
        }
        SECTION("RRR not to me") {
            q.add_rx_text("PU2GK EA0DX RRR", 12, &meta, &tx_msg);
        }
        SECTION("73 not to me") {
            q.add_rx_text("PU2GK EA0DX 73", 12, &meta, &tx_msg);
        }
        REQUIRE_THAT(tx_msg.msg, Equals("empty msg"));
        REQUIRE(tx_msg.repeats == default_repeats);
    }
    SECTION("Answer to GRID") {
        q.add_rx_text("R2RFE EA0DX KO12", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == -1);
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE +12"));
    }
    SECTION("Answer to report") {
        q.add_rx_text("R2RFE EA0DX -12", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == -1);
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE R+12"));
    }
    SECTION("Answer to R report") {
        q.add_rx_text("R2RFE EA0DX KK12", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX R+04", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == 1);
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE RR73"));
    }
    SECTION("Answer to RR73 report") {
        q.add_rx_text("R2RFE EA0DX -5", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX RR73", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == 1);
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE 73"));
    }
    SECTION("Answer to RRR report") {
        q.add_rx_text("R2RFE EA0DX -5", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX RRR", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == 1);
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE 73"));
    }
    qso_vec.clear();
}

TEST_CASE("Check no change tx_msg if not auto", "[ft8_qso]") {
    const int    default_repeats = -100;
    FTxQsoProcessor q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    q.set_auto(false);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg = {.msg = "prev message", .repeats = default_repeats};

    SECTION("Answer to GRID") {
        q.add_rx_text("R2RFE EA0DX KO12", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == default_repeats);
        REQUIRE_THAT(tx_msg.msg, Equals("prev message"));
    }
    SECTION("Answer to report") {
        q.add_rx_text("R2RFE EA0DX -12", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == default_repeats);
        REQUIRE_THAT(tx_msg.msg, Equals("prev message"));
    }
    SECTION("Answer to R report") {
        q.add_rx_text("R2RFE EA0DX KK12", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX R+04", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == default_repeats);
        REQUIRE_THAT(tx_msg.msg, Equals("prev message"));
    }
    SECTION("Answer to RR73 report") {
        q.add_rx_text("R2RFE EA0DX -5", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX RR73", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == default_repeats);
        REQUIRE_THAT(tx_msg.msg, Equals("prev message"));
    }
    SECTION("Answer to RRR report") {
        q.add_rx_text("R2RFE EA0DX -5", 12, &meta, &tx_msg);
        q.add_rx_text("R2RFE EA0DX RRR", 12, &meta, &tx_msg);
        REQUIRE(tx_msg.repeats == default_repeats);
        REQUIRE_THAT(tx_msg.msg, Equals("prev message"));
    }
    qso_vec.clear();
}

TEST_CASE("Check answer to many", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg = {.msg = "prev msg", .repeats = default_repeats};
    SECTION("answer to first") {
        SECTION("All new") {
            q.add_rx_text("R2RFE EA0DX KK12", -5, &meta, &tx_msg);
            REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE -05"));
            q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA2DX +05", 12, &meta, &tx_msg);
        }
        SECTION("Some without history") {
            q.add_rx_text("R2RFE EA1DX R+12", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA2DX RR73", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA3DX 73", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA0DX KK12", -5, &meta, &tx_msg);
        }
        REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE -05"));
    }
    SECTION("answer to next") {
        q.add_rx_text("R2RFE EA0DX KK12", -5, &meta, &tx_msg);
        q.start_new_slot();
        q.add_rx_text("R2RFE EA0DX R+12", -5, &meta, &tx_msg);
        q.start_new_slot();
        SECTION("73 first") {
            q.add_rx_text("R2RFE EA0DX 73", -5, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE +12"));
            q.add_rx_text("R2RFE EA2DX +05", 12, &meta, &tx_msg);
        }
        SECTION("73 in the middle") {
            q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA0DX 73", -5, &meta, &tx_msg);
            REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE +12"));
            q.add_rx_text("R2RFE EA2DX +05", 12, &meta, &tx_msg);
        }
        SECTION("73 last") {
            q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA2DX +05", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA0DX 73", -5, &meta, &tx_msg);
        }
        SECTION("many slots") {
            q.add_rx_text("R2RFE EA2DX +05", 12, &meta, &tx_msg);
            q.start_new_slot();
            q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("R2RFE EA0DX 73", -5, &meta, &tx_msg);
        }
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE +12"));
    }
    qso_vec.clear();
}

TEST_CASE("Check save qso", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg;
    SECTION("no save") {
        SECTION("Messages not to me") {
            q.add_rx_text("CQ EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX AB31", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX +05", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX R-12", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX RRR", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX RR73", 12, &meta, &tx_msg);
            q.add_rx_text("PU2GK EA1DX 73", 12, &meta, &tx_msg);
        }
        SECTION("unfinished") {
            SECTION("from grid") {
                q.add_rx_text("R2RFE EA1DX AB31", 12, &meta, &tx_msg);
            }
            SECTION("from report") {
                q.add_rx_text("R2RFE EA1DX +12", 12, &meta, &tx_msg);
            }
        }
        REQUIRE(qso_vec.size() == 0);
    }
    SECTION("save") {
        SECTION("from grid") {
            SECTION("singe report") {
                q.add_rx_text("R2RFE EA1DX AB31", 5, &meta, &tx_msg);
            }
            SECTION("last reports") {
                q.add_rx_text("R2RFE EA1DX AB31", 8, &meta, &tx_msg);
                q.start_new_slot();
                q.add_rx_text("R2RFE EA1DX AB31", 5, &meta, &tx_msg);
            }
            q.start_new_slot();
            q.add_rx_text("R2RFE EA1DX R+12", 7, &meta, &tx_msg);
            REQUIRE_THAT(std::get<1>(qso_vec[0]), Equals("AB31"));
        }
        SECTION("from single report") {
            q.add_rx_text("R2RFE EA1DX +12", 5, &meta, &tx_msg);
            q.start_new_slot();
            SECTION("RR73") {
                q.add_rx_text("R2RFE EA1DX RR73", 7, &meta, &tx_msg);
            }
            SECTION("RRR") {
                q.add_rx_text("R2RFE EA1DX RRR", 7, &meta, &tx_msg);
            }
            REQUIRE_THAT(std::get<1>(qso_vec[0]), Equals(""));
        }
        SECTION("from last report") {
            q.add_rx_text("R2RFE EA1DX +12", 8, &meta, &tx_msg);
            q.start_new_slot();
            q.add_rx_text("R2RFE EA1DX +12", 5, &meta, &tx_msg);
            q.start_new_slot();
            SECTION("RR73") {
                q.add_rx_text("R2RFE EA1DX RR73", 7, &meta, &tx_msg);
            }
            SECTION("RRR") {
                q.add_rx_text("R2RFE EA1DX RRR", 7, &meta, &tx_msg);
            }
            REQUIRE_THAT(std::get<1>(qso_vec[0]), Equals(""));
        }
        SECTION("don't save again") {
            q.add_rx_text("R2RFE EA1DX AB31", 5, &meta, &tx_msg);
            q.start_new_slot();
            q.add_rx_text("R2RFE EA1DX R+12", 7, &meta, &tx_msg);
            q.start_new_slot();
            q.add_rx_text("R2RFE EA1DX R+12", 7, &meta, &tx_msg);
            REQUIRE_THAT(std::get<1>(qso_vec[0]), Equals("AB31"));
        }
        REQUIRE(qso_vec.size() == 1);
        auto [remote_callsign, remote_grid, r_snr, s_snr] = qso_vec[0];
        REQUIRE_THAT(remote_callsign, Equals("EA1DX"));
        REQUIRE(r_snr == 12);
        REQUIRE(s_snr == 5);
    }
    qso_vec.clear();
}

TEST_CASE("Check manual start qso", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg;
    SECTION("answer to CQ") {
        q.add_rx_text("CQ EA1DX KK12", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE LO02"));
    }
    SECTION("Call another") {
        SECTION("msg GRID") {
            q.add_rx_text("B4BB EA1DX KK12", 7, &meta, &tx_msg);
        }
        SECTION("msg REPORT") {
            q.add_rx_text("B4BB EA1DX +07", 7, &meta, &tx_msg);
        }
        SECTION("msg R_REPORT") {
            q.add_rx_text("B4BB EA1DX R-02", 7, &meta, &tx_msg);
        }
        SECTION("msg RR73") {
            q.add_rx_text("B4BB EA1DX RR73", 7, &meta, &tx_msg);
        }
        SECTION("msg RRR") {
            q.add_rx_text("B4BB EA1DX RRR", 7, &meta, &tx_msg);
        }
        SECTION("msg 73") {
            q.add_rx_text("B4BB EA1DX 73", 7, &meta, &tx_msg);
        }
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE LO02"));
    }
    SECTION("answer to GRID") {
        q.add_rx_text("R2RFE EA1DX KK12", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE +07"));
    }
    SECTION("answer to REPORT") {
        q.add_rx_text("R2RFE EA1DX -12", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE R+07"));
    }
    SECTION("answer to R_REPORT") {
        q.add_rx_text("R2RFE EA1DX R-10", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE RR73"));
    }
    SECTION("answer to RR73") {
        q.add_rx_text("R2RFE EA1DX RR73", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE 73"));
    }
    SECTION("answer to RRR") {
        q.add_rx_text("R2RFE EA1DX RRR", 7, &meta, &tx_msg);
        tx_msg.msg[0] = '\0';
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE 73"));
    }
    SECTION("no answer") {
        SECTION("to me") {
            SECTION("73") {
                q.add_rx_text("R2RFE EA1DX 73", 7, &meta, &tx_msg);
            }
        }
        strcpy(tx_msg.msg, "SOME TEXT");
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("SOME TEXT"));
    }
    qso_vec.clear();
}

TEST_CASE("Check limit QTH len", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02rq", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg = {.msg = "PREV MSG", .repeats = default_repeats};
    q.add_rx_text("CQ EA0DX KK12", 7, &meta, &tx_msg);
    q.start_qso(&meta, &tx_msg);
    REQUIRE_THAT(tx_msg.msg, Equals("EA0DX R2RFE LO02"));
    qso_vec.clear();
}

TEST_CASE("Check reset", "[ft8_qso]") {
    const int      default_repeats = -100;
    FTxQsoProcessor   q = FTxQsoProcessor("R2RFE", "LO02rq", save_qso);
    ftx_msg_meta_t meta;
    ftx_tx_msg_t   tx_msg = {.msg = "PREV MSG", .repeats = default_repeats};
    q.add_rx_text("R2RFE EA0DX KK12", 7, &meta, &tx_msg);
    q.start_qso(&meta, &tx_msg);
    q.reset();
    q.add_rx_text("R2RFE EA1DX KK12", 7, &meta, &tx_msg);
    REQUIRE_THAT(tx_msg.msg, Equals("EA1DX R2RFE +07"));
    qso_vec.clear();
}

TEST_CASE("Non-standard own callsign", "[ft8_qso]") {
    FTxQsoProcessor q = FTxQsoProcessor("KF0QGT/6Y", "FK48", save_qso);
    ftx_msg_meta_t  meta;
    ftx_tx_msg_t    tx_msg = {.msg = "PREV MSG", .repeats = -100};
    q.set_local_standard(false);

    SECTION("answering a CQ: type 4, call in plain text, no grid") {
        q.add_rx_text("CQ W1ABC FN42", -12, &meta, &tx_msg);
        q.start_qso(&meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("<W1ABC> KF0QGT/6Y"));
        q.add_rx_text("<KF0QGT/6Y> W1ABC -05", -12, &meta, &tx_msg);
        REQUIRE(meta.to_me);
        REQUIRE_THAT(tx_msg.msg, Equals("W1ABC KF0QGT/6Y R-12"));
        q.add_rx_text("<KF0QGT/6Y> W1ABC RR73", -12, &meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("<W1ABC> KF0QGT/6Y 73"));
    }
    SECTION("after own CQ: reports as type 1, RR73 as type 4") {
        q.add_rx_text("<KF0QGT/6Y> W1ABC FN42", -8, &meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("W1ABC KF0QGT/6Y -08"));
        q.add_rx_text("<KF0QGT/6Y> W1ABC R-03", -8, &meta, &tx_msg);
        REQUIRE_THAT(tx_msg.msg, Equals("<W1ABC> KF0QGT/6Y RR73"));
    }
    qso_vec.clear();
}

TEST_CASE("Standard own callsign with /P keeps the grid", "[ft8_qso]") {
    FTxQsoProcessor q = FTxQsoProcessor("SP2KMO/P", "JO94", save_qso);
    ftx_msg_meta_t  meta;
    ftx_tx_msg_t    tx_msg = {.msg = "PREV MSG", .repeats = -100};
    q.add_rx_text("CQ W1ABC FN42", -12, &meta, &tx_msg);
    q.start_qso(&meta, &tx_msg);
    REQUIRE_THAT(tx_msg.msg, Equals("W1ABC SP2KMO/P JO94"));
    qso_vec.clear();
}
