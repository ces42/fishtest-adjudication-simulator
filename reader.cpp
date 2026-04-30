#include <cassert>
#include <stdfloat>
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include <charconv>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <numeric>

// #include "fast_double_parser.h"
// #include "fast_float_t.h"

// Enum to represent the outcome of the game
enum GameResult : uint8_t {
    WhiteWin,  // 1-0
    Draw,      // 1/2-1/2
    BlackWin,  // 0-1
    Unknown    // * or unparsed
};

enum GameTerm : uint8_t {
    Adj,
    Normal,
    Time,
    Other,
};

using float_t = float;

// Structure to hold data for a single game
struct GameData {
    std::vector<int16_t> scores;
    std::vector<uint> times;
    GameResult result = GameResult::Unknown;
    GameTerm term = GameTerm::Other;
    GameResult alt_result = GameResult::Unknown;
    bool white_is_new = true;
    int early_time = -1;
    int plyCount;
};

// Helper function to convert the enum back to a string for display
std::string res2str(GameResult res) {
    switch (res) {
        case GameResult::WhiteWin: return "1-0";
        case GameResult::BlackWin: return "0-1";
        case GameResult::Draw:     return "draw";
        default:                   return "NONE";
    }
}

std::string res2str_nb(GameResult res) {
    switch (res) {
        case GameResult::WhiteWin: return "new";
        case GameResult::BlackWin: return "base";
        case GameResult::Draw:     return "draw";
        default:                   return "NONE";
    }
}

std::string termToString(GameTerm term) {
    switch (term) {
        case GameTerm::Adj: return "adjudicated";
        case GameTerm::Normal: return "chess";
        case GameTerm::Time:  return "time loss";
        default:                   return "* (Unknown)";
    }
}

inline std::pair<bool, float_t> try_parse_f(const std::string_view& str, size_t start, size_t end) {
    float_t ret;
    // __attribute__((assume(end - start < 10)));
    auto res = std::from_chars(str.data() + start + (str[start] == '+'),
                               str.data() + end, ret,
                               std::chars_format::fixed);
    // assert(res.ptr == str.c_str() + end);
    return {res.ec == std::errc(), ret};
}

inline float_t parse_f(const std::string_view& str, size_t start, size_t end) {
    auto [succ, res] = try_parse_f(str, start, end);
    // assert(succ);
    return res;
}

inline int fixed_unsigned_stoi(const char * str, int decimals = 3) {
    int val = 0;
    while (*str != '.') {
        // assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    ++str;
    for (int j = 0; j < decimals; ++j) {
        // assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    return val;
}

inline int16_t fixed_signed_stoi(const char * str, int decimals = 2) {
    int16_t sign = 1;
    if (*str == '+') {
        ++str;
        if (*str == 'M')
            return 255;
    } else if(*str == '-') {
        sign = -1;
        ++str;
        if (*str == 'M')
            return -255;
    }
    int16_t val = 0;
    while (*str != '.') {
        // assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    ++str;
    for (int j = 0; j < decimals; ++j) {
        // assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    return sign * val;
}

constexpr bool TRACK_MOVES = false;

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cerr << "Usage: " << argv[0] << " <filename.pgn> <draw threshold> <draw plies> <resign threshold> <resign plies>\n";
        return 1;
    }

    // 1. Open the file using low-level POSIX open()
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error: Could not open file\n";
        return 1;
    }

    // 2. Get the exact size of the file
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        std::cerr << "Error: Could not get file size\n";
        close(fd);
        return 1;
    }

    // 3. Memory-map the file
    // PROT_READ means we only want to read. MAP_PRIVATE means changes aren't written to disk.
    const char* file_data = static_cast<const char*>(mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (file_data == MAP_FAILED) {
        std::cerr << "Error: mmap failed: " << errno << std::endl;
        close(fd);
        return 1;
    }
    madvise((void*)file_data, sb.st_size, MADV_SEQUENTIAL | MADV_HUGEPAGE);

    // 4. We can safely close the file descriptor now; the mapping remains active
    close(fd);

    const char* cursor = file_data;
    const char* const end_ptr = file_data + sb.st_size;
    auto stream_find = [&end_ptr] (const char * ptr, char c) {
        return (const char*) memchr(ptr, c, end_ptr - ptr);
    };

    std::vector<GameData> games;
    games.reserve(100);
    GameData* currentGame = &games.emplace_back();
    // GameData currentGame = GameData();
    int len;
    float q;
    constexpr uint TIME_QUANT = 1024;
    uint qq;
    // int min_len = 999999;
    int mispredicts = 0;
    uint64_t savings = 0;
    uint64_t tot_time = 0;

    int draw_th  = std::stoi(argv[2]);
    int draw_len = std::stoi(argv[3]);
    int win_th   = std::stoi(argv[4]);
    int win_len  = std::stoi(argv[5]);

    int64_t error_matrix[3][3] = { 0 };
    int64_t match_matrix[3][3] = { 0 };

    bool seen_header = false;
    // read the file delimited by header lines
    // while (std::getline(file, line, '[')) {
    while (cursor < end_ptr) {

        // Create a string_view representing this specific block of the file
        // std::string_view line(cursor + 1, length);

        if (*cursor == '[') {
            cursor++;
            std::string_view str(cursor, std::min(4096L, end_ptr - cursor - 1));
            if (str[0] == 'T' && str.starts_with("TimeControl \"")) {
                float tc;
                size_t end = str.find('+', 13);
                cursor += 13;
                std::from_chars(str.data() + 13, str.data() + end, tc);
                q = 60.0 / tc;
                qq = int(TIME_QUANT * q);
            } else if (str[0] == 'W' && str.starts_with("White \"")) {
                cursor += 11;
                if (str.substr(7, 4) == "New-") {
                    currentGame->white_is_new = true;
                } else {
                    currentGame->white_is_new = false;
                }
            } else if (str[0] == 'R' && str.starts_with("Result \"")) {
                std::string_view res_str = str.substr(8, 4);
                cursor += 12;
                if (res_str == "1-0\"") {
                    currentGame->result = GameResult::WhiteWin;
                } else if (res_str == "0-1\"") {
                    currentGame->result = GameResult::BlackWin;
                } else if (res_str == "1/2-") {
                    currentGame->result = GameResult::Draw;
                } else {
                    assert(false);
                }

            } else if (str[0] == 'T' && str.starts_with("Termination \"")) {
                size_t end = str.find('"', 13);
                std::string_view res_str = str.substr(13, end - 13);
                cursor += 13;
                if (res_str == "adjudication")
                    currentGame->term = GameTerm::Adj;
                else if (res_str == "normal")
                    currentGame->term = GameTerm::Normal;
                else if (res_str == "time forfeit")
                    currentGame->term = GameTerm::Time;
                else
                    currentGame->term = GameTerm::Other;

            } else if (str[0] == 'P' && str.starts_with("PlyCount \"")) {
                size_t end = str.find('"', 10);
                cursor += 10;
                std::from_chars(str.data() + 10, str.data() + end, len);
                currentGame->plyCount = len;
                seen_header = true;
                if constexpr (TRACK_MOVES) {
                    currentGame->scores.reserve(len);
                    currentGame->times.reserve(len);
                }
            }
            cursor = stream_find(cursor, '\n') + 1;
        } else if (seen_header && *cursor == '\n') {
            cursor++;
            // size_t pos = 0;
            // std::string_view str(cursor+1, std::min(1024 * 1024L, end_ptr - cursor - 1));
            // std::cout << std::string_view(cursor+1, size_t(10)) << std::endl;
            bool white_to_move = !(std::string_view(cursor + 2, 2) == "..");
            int sign = white_to_move ? 1 : -1;
            uint cumtime = 0;

            int draw_plies = 0;
            int win_plies = 0;
            int lose_plies = 0;
            int plies = 0;

            GameResult& alt_result = currentGame->alt_result;

            // Search for comments wrapped in '{' and '}'
            // pos = str.find('{', pos);
            while (plies < currentGame->plyCount) {
                cursor = stream_find(cursor, '{') + 1;
                // maybe a short loop could be faster here...
                // for (; *cursor != '{'; ++cursor);
                // ++cursor;
                // ... but I'm measuring a 15% perf decrease

                // the shortest possible score is a single-digit mate score, it has length 3
                const char * end;
                for (end = cursor + 3; *end != '/'; ++end);

                // size_t end = pos + 1 + 5;
                // auto [succ, score] = try_parse_f(line, pos+1, end);
                // if (!succ) [[unlikely]] {
                //     if (line_view.substr(pos + 1, 2) == "+M")
                //         score = +255;
                //     else if (line_view.substr(pos + 1, 2) == "-M")
                //         score = -255;
                //     else
                //         assert(false);
                // }
                int16_t score = fixed_signed_stoi(cursor, 2);
                cursor = end + 1; // +1 to move past "/"

                // +1 because depth is at least one digit
                if (*(cursor + 2) == ' ')
                    end = cursor + 2;
                else [[unlikely]]
                    for (end = cursor + 1; *end != ' '; ++end);


                // int depth = std::stoi(line.substr(pos, end - pos));
                // int depth;
                // std::from_chars(line.c_str() + pos, line.c_str() + end, depth);
                cursor = end + 1; // +1 to move past " "

                // end = line.find('s', pos);
                // time is at least 5 digits
                for (end = cursor + 5; *end != 's'; ++end);

                // end = pos + 5;
                // float_t time = parse_f(line, pos, end);
                uint time = fixed_unsigned_stoi(cursor, 3);
                cumtime += time * qq;
                cursor = end + 6; // "s} __ {"

                // std::cout << "> after time: " << std::string_view(end, 6) << "#\n";
                if constexpr (TRACK_MOVES) {
                    currentGame->scores.push_back(score);
                    currentGame->times.push_back(cumtime / TIME_QUANT);
                }
                ++plies;

                if (alt_result == Unknown) {
                    if (score * sign >= win_th) {
                        win_plies += 1;
                        if (win_plies >= win_len) {
                            alt_result = WhiteWin;
                            currentGame->early_time = cumtime / TIME_QUANT;
                        }
                    } else
                        win_plies = 0;

                    if (score * sign <= -win_th) {
                        lose_plies += 1;
                        if (lose_plies >= win_len) {
                            alt_result = BlackWin;
                            currentGame->early_time = cumtime / TIME_QUANT;
                        }
                    } else
                        lose_plies = 0;

                    if (std::abs(score) <= draw_th) {
                        draw_plies += 1;
                        if (draw_plies >= draw_len) {
                            alt_result = Draw;
                            currentGame->early_time = cumtime / TIME_QUANT;
                        }
                    } else
                        draw_plies = 0;
                }
                sign *= (-1);
            }
            cursor = stream_find(cursor, '[');
            if (!cursor) [[unlikely]] cursor = end_ptr;

            if (alt_result != Unknown) {
                error_matrix[currentGame->result][alt_result]++;

                int actual_nb = currentGame->result;
                int alt_nb = alt_result;
                if (!currentGame->white_is_new) {
                    if (actual_nb == WhiteWin) actual_nb = BlackWin;
                    else if (actual_nb == BlackWin) actual_nb = WhiteWin;
                    if (alt_nb == WhiteWin) alt_nb = BlackWin;
                    else if (alt_nb == BlackWin) alt_nb = WhiteWin;
                }
                match_matrix[actual_nb][alt_nb]++;

                if (alt_result != currentGame->result) {
                    mispredicts++;
                }
                savings += cumtime / TIME_QUANT - currentGame->early_time;
            } else {
                // std::cout << "hereXX" << currentGame->result << alt_result << "\n";
                error_matrix[currentGame->result][currentGame->result]++;
                int actual_nb = currentGame->result;
                if (!currentGame->white_is_new) {
                    if (actual_nb == WhiteWin) actual_nb = BlackWin;
                    else if (actual_nb == BlackWin) actual_nb = WhiteWin;
                }
                match_matrix[actual_nb][actual_nb]++;
            }
            tot_time += cumtime / TIME_QUANT;
            // assert(currentGame->data.size() == len);
            currentGame = &games.emplace_back();
            seen_header = false;
        }
    }

    games.pop_back();

    if constexpr (TRACK_MOVES)
    {
        for (size_t i = 0; i < std::min(games.size(), 10UL); ++i) {
            std::cout << "Game " << (i + 1)
                      << " | Result: " << res2str(games[i].result) 
                      << " | alt result: " << res2str(games[i].alt_result) 
                      << " | at time: " << games[i].early_time 
                      << " | Termination: " << termToString(games[i].term) 
                      << "\n";
                      // << " | Total Moves Extracted: " << games[i].scores.size() << "\n";

            // Print the first few extractions as a sanity check
            int display_count = games[i].scores.size();
            for (int j = std::max(0, display_count - 20); j < display_count; ++j) {
                std::cout << "  " << j << "  " <<
                    (j % 2 ? -1 : 1) * games[i].scores[j] << "  " <<
                    games[i].times[j] / 1000.0 << "\n";
            }
            std::cout << "\n";
        }
    }
    std::cout << "Parameters: ";
    std::cout << "draw(" << draw_th << "cp, " << draw_len << " plies)" << "   ";
    std::cout << "win(" << win_th << "cp, " << win_len << " plies)\n";


    double save_per = double(savings) / 1000.0 / games.size();
    double avg_game_dur = double(tot_time) / 1000.0 / games.size();
    std::cout << "Parsed " << games.size() << " games.\n";
    std::cout << "avg game duration : " << avg_game_dur << "s\n\n";
    std::cout << "mispredicts: " << mispredicts << "   " << mispredicts * 100.0 / games.size() << "%\n";
    std::cout << "avg time saved: " << save_per << "s  " << 
        save_per / avg_game_dur * 100.0 << "%\n";
    // std::cout << "shortest game: " << min_len << "moves\n";

    std::cout << std::endl << std::left << std::setw(8) << "actual";
    for (int res = 0; res < 3; ++res)
        std::cout << "  " << std::left << std::setw(8) << res2str(GameResult(res));
    std::printf("\n");
    std::cout << "----------------------------------------";
    std::printf("\n");
    std::cout << std::setprecision(3);
    int i = 0;
    for (auto& row : error_matrix) {
        std::cout << std::left << std::setw(8) << res2str(GameResult(i));
        int64_t tot = row[0] + row[1] + row[2];
        for (int64_t& e : row)
            std::cout << "  " << std::setw(8) << (tot ? e * 1.0 / tot * 100 : 0.0);
        std::cout << std::endl;
        ++i;
    }

    // std::cout << "\nNew vs Base matrix:\n";
    std::cout << "\n";
    std::cout << std::left << std::setw(8) << "actual";
    for (int res = 0; res < 3; ++res)
        std::cout << "  " << std::left << std::setw(8) << res2str_nb(GameResult(res));
    std::printf("\n");
    std::cout << "----------------------------------------";
    std::printf("\n");
    i = 0;
    for (auto& row : match_matrix) {
        std::cout << std::left << std::setw(8) << res2str_nb(GameResult(i));
        int64_t tot = row[0] + row[1] + row[2];
        for (int64_t& e : row)
            std::cout << "  " << std::setw(8) << (tot ? e * 1.0 / tot * 100 : 0.0);
        std::cout << std::endl;
        ++i;
    }

    int wins   = std::accumulate(match_matrix[0], match_matrix[1], 0);
    int draws  = std::accumulate(match_matrix[1], match_matrix[2], 0);
    int losses = std::accumulate(match_matrix[2], match_matrix[3], 0);
    std::cout << "\n"
        << "[" << wins << ", "
        << draws << ", "
        << losses << "]"
        << "  -->  "
        << "[" << match_matrix[0][0] + match_matrix[1][0] + match_matrix[2][0] << ", "
        << match_matrix[0][1] + match_matrix[1][1] + match_matrix[2][1] << ", "
        << match_matrix[0][2] + match_matrix[1][2] + match_matrix[2][2] << "]"
        << "\n";

    double error = double(-50 * match_matrix[0][1] - 100 * match_matrix[0][2]
        + 50 * match_matrix[1][0] - 50 * match_matrix[1][2]
        + 100 * match_matrix[2][0] + 50 * match_matrix[2][1]) / games.size();

    std::cout <<  std::fixed << std::right;
    std::cout << "match outcome for new: " << std::setprecision(4) << std::setw(7)
        << 50.0 * (wins - losses) / games.size() << " %\n";
    std::cout << "match error for new:   " << std::setprecision(4) << std::setw(7)
        << error << " %\n";

    munmap((void*)file_data, sb.st_size);
    return 0;
}
