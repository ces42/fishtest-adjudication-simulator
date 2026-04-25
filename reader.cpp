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
#include <iomanip>

#include "npy.hpp"

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
    int early_time = -1;
    bool white_is_new = true;
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
    madvise((void*)file_data, sb.st_size, MADV_SEQUENTIAL);

    // 4. We can safely close the file descriptor now; the mapping remains active
    close(fd);

    const char* cursor = file_data;
    const char* end_ptr = file_data + sb.st_size;

    std::vector<GameData> games;
    games.reserve(100);
    GameData* currentGame = &games.emplace_back();
    // GameData currentGame = GameData();
    std::string line;
    int len;
    float q;
    constexpr uint TIME_QUANT_SCALE = 1024;
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
    int64_t new_base_matrix[3][3] = { 0 };

    // read the file delimited by header lines
    // while (std::getline(file, line, '[')) {
    while (cursor < end_ptr) {
        const char* next_bracket = (const char*)(memchr(cursor + 1, '[', end_ptr - cursor - 1));
        if (!next_bracket) {
            next_bracket = end_ptr;
        }

        // Create a string_view representing this specific block of the file
        size_t length = next_bracket - cursor;
        std::string_view line(cursor + 1, length);
        cursor = next_bracket;

        size_t pos;
        if (line.starts_with("TimeControl \"")) {
            float tc;
            size_t end = line.find('+', 13);
            std::from_chars(line.data() + 13, line.data() + end, tc);
            q = 60.0 / tc;
            qq = int(1024 * q);
        }
        if (line.starts_with("White \"")) {
            if (line.substr(7, 4) == "New-") {
                currentGame->white_is_new = true;
            } else {
                currentGame->white_is_new = false;
            }
        } else if (line.starts_with("Result \"")) {
            std::string_view res_str = line.substr(8, 4);
            if (res_str == "1-0\"") {
                currentGame->result = GameResult::WhiteWin;
            } else if (res_str == "0-1\"") {
                currentGame->result = GameResult::BlackWin;
            } else if (res_str == "1/2-") {
                currentGame->result = GameResult::Draw;
            } else {
                assert(false);
            }

        } else if (line.starts_with("Termination \"")) {
            size_t end = line.find('"', 13);
            std::string_view res_str = line.substr(13, end - 13);
            if (res_str == "adjudication")
                currentGame->term = GameTerm::Adj;
            else if (res_str == "normal")
                currentGame->term = GameTerm::Normal;
            else if (res_str == "time forfeit")
                currentGame->term = GameTerm::Time;
            else
                currentGame->term = GameTerm::Other;

        } else if (line.starts_with("PlyCount \"")) {
            size_t end = line.find('"', 10);
            std::from_chars(line.data() + 10, line.data() + end, len);
            currentGame->scores.reserve(len);
            currentGame->times.reserve(len);
        } else if ((pos = line.find("\n\n")) != std::string_view::npos) {
            pos = line.find("\n\n");
            bool white_to_move = !(line.substr(pos + 2 + 2, 2) == "..");
            int sign = white_to_move ? 1 : -1;
            uint cumtime = 0;

            int draw_plies = 0;
            int win_plies = 0;
            int lose_plies = 0;

            GameResult& alt_result = currentGame->alt_result;

            // Search for comments wrapped in '{' and '}'
            while ((pos = line.find('{', pos)) != std::string_view::npos) {

                size_t end = line.find('/', pos+1);
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
                int16_t score = fixed_signed_stoi(line.data() + pos + 1, 2);
                assert(end + 1 > pos);
                pos = end + 1; // "/"

                end = line.find(' ', pos);
                // int depth = std::stoi(line.substr(pos, end - pos));
                // int depth;
                // std::from_chars(line.c_str() + pos, line.c_str() + end, depth);
                pos = end + 1; // " "

                end = line.find('s', pos);
                // end = pos + 5;
                // float_t time = parse_f(line, pos, end);
                uint time = fixed_unsigned_stoi(line.data() + pos, 3);
                cumtime += time * qq;
                pos = end + 6; // "s} __ "
                // currentGame->scores.push_back(score);
                // currentGame->times.push_back(cumtime / TIME_QUANT_SCALE);

                if (alt_result == Unknown) {
                    if (score * sign >= win_th) {
                        win_plies += 1;
                        if (win_plies >= win_len) {
                            alt_result = WhiteWin;
                            currentGame->early_time = cumtime / TIME_QUANT_SCALE;
                        }
                    } else
                        win_plies = 0;

                    if (score * sign <= -win_th) {
                        lose_plies += 1;
                        if (lose_plies >= win_len) {
                            alt_result = BlackWin;
                            currentGame->early_time = cumtime / TIME_QUANT_SCALE;
                        }
                    } else
                        lose_plies = 0;

                    if (std::abs(score) <= draw_th) {
                        draw_plies += 1;
                        if (draw_plies >= draw_len) {
                            alt_result = Draw;
                            currentGame->early_time = cumtime / TIME_QUANT_SCALE;
                        }
                    } else
                        draw_plies = 0;
                }
                sign *= (-1);
            }
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
                new_base_matrix[actual_nb][alt_nb]++;

                if (alt_result != currentGame->result) {
                    mispredicts++;
                }
                savings += cumtime / TIME_QUANT_SCALE - currentGame->early_time;
            } else {
                error_matrix[currentGame->result][currentGame->result]++;
                int actual_nb = currentGame->result;
                if (!currentGame->white_is_new) {
                    if (actual_nb == WhiteWin) actual_nb = BlackWin;
                    else if (actual_nb == BlackWin) actual_nb = WhiteWin;
                }
                new_base_matrix[actual_nb][actual_nb]++;
            }
            tot_time += cumtime / TIME_QUANT_SCALE;
            // assert(currentGame->data.size() == len);
            currentGame = &games.emplace_back();
        }
        // cursor = cursor + 1 /* `line` start here */ + pos;
    }

    games.pop_back();

    // --- Output results to verify extraction ---
    // for (size_t i = 0; i < std::min(games.size(), 10UL); ++i) {
    //     std::cout << "Game " << (i + 1)
    //               << " | Result: " << res2str(games[i].result) 
    //               << " | alt result: " << res2str(games[i].alt_result) 
    //               << " | at time: " << games[i].early_time 
    //               << " | Termination: " << termToString(games[i].term) 
    //               << "\n";
    //               // << " | Total Moves Extracted: " << games[i].scores.size() << "\n";
    //
    //     // Print the first few extractions as a sanity check
    //     int display_count = games[i].scores.size();
    //     for (int j = std::max(0, display_count - 20); j < display_count; ++j) {
    //         std::cout << "  " << j << "  " <<
    //             (j % 2 ? -1 : 1) * games[i].scores[j] << "  " <<
    //             games[i].times[j] << "\n";
    //     }
    //     std::cout << "\n";
    // }
    std::cout << "Draw: " << draw_th << ", " << draw_len << "\n";
    std::cout << "Win: " << win_th << ", " << win_len << "\n";


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

    std::cout << "\nNew vs Base matrix:\n";
    std::cout << std::left << std::setw(8) << "actual";
    for (int res = 0; res < 3; ++res)
        std::cout << "  " << std::left << std::setw(8) << res2str_nb(GameResult(res));
    std::printf("\n");
    std::cout << "----------------------------------------";
    std::printf("\n");
    i = 0;
    for (auto& row : new_base_matrix) {
        std::cout << std::left << std::setw(8) << res2str_nb(GameResult(i));
        int64_t tot = row[0] + row[1] + row[2];
        for (int64_t& e : row)
            std::cout << "  " << std::setw(8) << (tot ? e * 1.0 / tot * 100 : 0.0);
        std::cout << std::endl;
        ++i;
    }


    munmap((void*)file_data, sb.st_size);
    return 0;
}
