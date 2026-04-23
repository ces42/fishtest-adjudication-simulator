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

// #include "fast_double_parser.h"
// #include "fast_float_t.h"

// Enum to represent the outcome of the game
enum GameResult {
    WhiteWin,  // 1-0
    BlackWin,  // 0-1
    Draw,      // 1/2-1/2
    Unknown    // * or unparsed
};

enum GameTerm {
    Adj,
    Normal,
    Time,
    Other,
};

using float_t = float;

// Structure to hold data for a single game
struct GameData {
    GameResult result = GameResult::Unknown;
    GameTerm term = GameTerm::Other;
    std::vector<int16_t> scores;
    std::vector<int> times;
};

// Helper function to convert the enum back to a string for display
std::string resultToString(GameResult res) {
    switch (res) {
        case GameResult::WhiteWin: return "1-0 (White Win)";
        case GameResult::BlackWin: return "0-1 (Black Win)";
        case GameResult::Draw:     return "1/2-1/2 (Draw)";
        default:                   return "* (Unknown)";
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

inline std::pair<bool, float_t> try_parse_f(const std::string& str, size_t start, size_t end) {
    float_t ret;
    // __attribute__((assume(end - start < 10)));
    auto res = std::from_chars(str.c_str() + start + (str[start] == '+'),
                               str.c_str() + end, ret,
                               std::chars_format::fixed);
    // assert(res.ptr == str.c_str() + end);
    return {res.ec == std::errc(), ret};
}

inline float_t parse_f(const std::string& str, size_t start, size_t end) {
    auto [succ, res] = try_parse_f(str, start, end);
    // assert(succ);
    return res;
}

inline int fixed_unsigned_stoi(const char * str, int decimals = 3) {
    int val = 0;
    while (*str != '.') {
        assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    ++str;
    for (int j = 0; j < decimals; ++j) {
        assert(*str - '0' >= 0);
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
        assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    ++str;
    for (int j = 0; j < decimals; ++j) {
        assert(*str - '0' >= 0);
        val = val * 10 + (*str++ - '0');
    }
    return sign * val;
}

int main(int argc, char* argv[]) {

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename.pgn>\n";
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

    // read the file delimited by header lines
    // while (std::getline(file, line, '[')) {
    while (cursor < end_ptr) {
        const char* next_bracket = static_cast<const char*>(memchr(cursor + 1, '[', end_ptr - cursor - 1));
        if (!next_bracket) {
            next_bracket = end_ptr;
        }

        // Create a string_view representing this specific block of the file
        size_t length = next_bracket - cursor;
        std::string_view line(cursor + 1, length);
        cursor = next_bracket;

        // // Detect the start of a new game
        // if (line[0] == '[') {
        size_t pos;
        // If the current game has data, save it before starting a new one
        if (line.starts_with("Result \"")) {
            std::string_view res_str = line.substr(8, 4);
            if (res_str == "1-0\"") {
                currentGame->result = GameResult::WhiteWin;
            } else if (res_str == "0-1\"") {
                currentGame->result = GameResult::BlackWin;
            } else if (res_str == "1/2-") {
                currentGame->result = GameResult::Draw;
            } else {
                currentGame->result = GameResult::Unknown;
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
        // } else if (line.substr(0, 13) == "TimeControl \"") {
            // the last line of the header is always TimeControl
        } else if ((pos = line.find("\n\n")) != std::string_view::npos) {

            // Search for comments wrapped in '{' and '}'
            while ((pos = line.find('{', pos)) != std::string_view::npos) {

                // // Fast format extraction: matches "<score>/<depth> <time>s"
                // if (std::sscanf(comment.c_str(), "%f/%d %fs", &score, &depth, &time) == 3) {
                //     currentGame->evals.push_back({score, time});
                // }
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
                int time = fixed_unsigned_stoi(line.data() + pos, 3);
                pos = end + 6; // "s} __ "
                currentGame->scores.push_back(score);
                currentGame->times.push_back(time);
            }
            // assert(currentGame->data.size() == len);
            currentGame = &games.emplace_back();
        }
    }

    games.pop_back();

    // --- Output results to verify extraction ---
    std::cout << "Parsed " << games.size() << " games.\n\n";
    for (size_t i = 0; i < std::min(games.size(), 20UL); ++i) {
        std::cout << "Game " << (i + 1)
                  << " | Result: " << resultToString(games[i].result) 
                  << " | Termination: " << termToString(games[i].term) 
                  << " | Total Moves Extracted: " << games[i].scores.size() << "\n";

        // Print the first few extractions as a sanity check
        int display_count = games[i].scores.size();
        for (int j = std::max(0, display_count - 20); j < display_count; ++j) {
            std::cout << "  " << (j % 2 ? -1 : 1) * games[i].scores[j] << "  " << games[i].times[j] << "\n";
                      // << ", " << games[i].evals[j].time << "s;";
        }
        std::cout << "\n";
    }

    munmap((void*)file_data, sb.st_size);
    return 0;
}
