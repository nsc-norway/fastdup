#include <utility>

#include <fstream>
#include <iostream>
#include <iomanip>

#include <unordered_map>
#include <forward_list>

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/program_options.hpp>

// gzip compatibility: gzip from Boost 1.48 does not support block gzip format
// (bgzf), so we include a local header file with support for it.
#if BOOST_VERSION == 104800
#include "gzip.hpp"
#else
#include <boost/iostreams/filter/gzip.hpp>
#endif


/*
 * suprDUPr - the duplicate detection tool
 *
 * This source file contains the entire suprDUPr program. The main function
 * is located at the bottom of the file.
 *
 * The code can be built in two different ways, depending on the value 
 * of the preprocessor macro named OUTPUT_READ_ID. If this macro is set 
 * to true, the program is modified to output read-identifier strings instead
 * of just counting.
 */

// This constant contains the maximum length of each line in the input file,
// used for the buffer size.
#define MAX_LEN 1024

using namespace std;
namespace po = boost::program_options;


// TwoBitSequence:
// Class to hold a fixed length nucleotide sequence, in an efficient two-bit
// encoding. It's mainly use for equality comparisons, so it has no method to
// recover the original string.
template <size_t N>
class TwoBitSequence {
 
public:
    // SequenceBuffer type -- Used as a buffer for data input.
    // The buffer before and after the member data are used for parts of the
    // sequence we are not interested in. The char_data member is used to get
    // a pointer to the data array, but as a character type.
    struct SequenceBuffer {
        char buffer[MAX_LEN];
        union {
            unsigned long data[N*4];
            char char_data;
        };
        char buffer2[MAX_LEN];
    };

    unsigned long data[N] = {};
    // Have half the number of unk's for N bases, but round up
    unsigned long unk[(N+1)/2] = {};
    
    inline TwoBitSequence(const unsigned long* blocks) {
        /* Conversion of ASCII string to 2-bit codes + unknown (N) flag:
         *        vv
         *  A 1000001
         *  C 1000011
         *  G 1000111
         *  T 1010100
         *  N 1001110
         *
         * So we use a bit mask of 0x06 = 00000110 to fish out these bits, and
         * then shift them as appropriate.
         *
         * The 2-bit codes aren't stored in order, making it a bit more efficient
         * to read them in 8-byte chunks. One can fit 4 2-bit base pairs into the
         * space of a single ASCII character (byte). The data are stored in
         * an unique, but non-obvious order (order is irrelevant as long as equality
         * is preserved):
         * 1,9,17,25,2,10,18,26,3,11,19,27,.....,8,16,24,32
         *
         * The sequence length is not needed. It is the responsibility of the 
         * caller to call the TwoBitSequence<N> with the correct N, where N
         * is seq_len/32 rounded up. Also, the buffer passed to the constructor
         * must be at least N*32 bytes long, zero padded if necessary.
         *
         * The handling of N characters in addition to ACGT was added later.
         */
        //const unsigned long* blocks = (const unsigned long*) __builtin_assume_aligned(seq, 8);
        for (size_t i=0; i<N; ++i) {
            // Read four blocks of ASCII data into the i-th 64-bit int
            data[i] = (blocks[i*4] & (0x0606060606060606ul)) >> 1;
            size_t j;
            for (j=0; j<3; ++j) {
                data[i] |= (blocks[i*4+j+1] & (0x0606060606060606ul)) << (j*2+1);
            }

            // Dealing with N characters: every two iterations of this loop share
            // one entry in the unk array. This adds a different offset for even
            // and odd iterations.
            size_t jshift, nshift;
            j = 0;
            if ((i & 1) == 1)
                nshift = 4;
            else
                nshift = 0;
            jshift = nshift;
            // Load four blocks of Ns into the current unk entry
            for (; jshift<3; ++jshift) {
                unk[i/2] |= (blocks[i*4+(j++)] & (0x0808080808080808ul)) >> (3-jshift);
            }
            for (; jshift<4+nshift; ++jshift) {
                unk[i/2] |= (blocks[i*4+(j++)] & (0x0808080808080808ul)) << (jshift-3);
            }
        }
    }

    inline bool operator==(const TwoBitSequence& other) const {
        for (size_t i=0; i<N; ++i) {
            if (data[i] != other.data[i]) 
                return false;
        }
        for (size_t i=0; i<(N+1)/2; ++i) {
            if (unk[i] != other.unk[i])
                return false;
        }
        return true;
    }

    // Computes a simple, fast hash of the sequence.
    inline size_t hash() const {
        // Hash ignores unk; treats N as G. As N is uncommon, it's 
        // not worth the effort.
        size_t hash = 0;
        for (size_t i=0; i<N; ++i) {
            hash += data[i];
        }
        return hash;
    }
};


// Entry:
// This class represents a single sequence read at a specific position 
// inside a physical region (tile). It holds the coordinates and the 
// sequence, and in case of the read-ID mode, it holds the read-ID 
// (FASTQ header) string.
template<typename VALUE>
class Entry {
    public:
        Entry* next = nullptr;
        short group;
        int x, y;
        VALUE value;
#ifdef OUTPUT_READ_ID
        string id;

        Entry(short group, int x, int y, const char* id, size_t idlen, const unsigned long* seq) :
            group(group), x(x), y(y), value(seq), id(id, idlen) {
        }
#else
        Entry(short group, int x, int y, const unsigned long* seq) :
            group(group), x(x), y(y), value(seq) {
        }
#endif

};

// Metrics is used to pass results from the analysisLoop function back
// into the main program.
class Metrics {
    // Collects totals
    public:
        bool error = false;
        unsigned long reads_with_duplicates = 0;
        unsigned long num_reads = 0;
};

/* The AnalysisHead class receives read one by one from the analysis loop,
 * and manages the processing of rows, and groups (tiles). The enterPoint
 * function is the critical piece of code, which checks for duplicates. */
template<typename VALUE>
class AnalysisHead {

    ostream& outout;

    const size_t hash_size;
    const size_t mask;
    int winx, winy;
    const bool region_sorted, unsorted;
    size_t str_len;

    typedef Entry<VALUE> Ent;
    Ent** data = nullptr;

    public:
    Metrics metrics;
        
        AnalysisHead(ostream& outout, 
                size_t hash_bytes, unsigned int winx, unsigned int winy, bool region_sorted,
                bool unsorted)
            : outout(outout),
                hash_size(hash_bytes/sizeof(Ent*)), mask(hash_size-1),
                winx(winx), winy(winy), region_sorted(region_sorted), unsorted(unsorted) {
            data = new Ent*[hash_size];
        }

        ~AnalysisHead() {
            delete data;
        }

#ifdef OUTPUT_READ_ID
        void enterPoint(int group, int x, int y, const char* id, size_t idlen, const unsigned long* seq) {
            Ent* new_entry = new Ent(group,x,y,id,idlen,seq);
#else
        void enterPoint(int group, int x, int y, const unsigned long* seq) {
            Ent* new_entry = new Ent(group,x,y,seq);
#endif
            Ent** entry_ptr = &data[new_entry->value.hash() & mask];
            bool any_duplicate_found = false;
            while (*entry_ptr) {
                Ent* entry = (*entry_ptr);
                if ((y - entry->y) > winy) {
                    if (unsorted || region_sorted) {
                        entry_ptr = &entry->next;
                        continue;
                    }
                    *entry_ptr = entry->next;
                    delete entry;
                }
                else if (entry->group != group) {
                    if (unsorted) {
                        entry_ptr = &entry->next;
                        continue;
                    }
                    *entry_ptr = entry->next;
                    delete entry;
                }
                else {
#ifdef OUTPUT_READ_ID
                    // Code path to output the read-ID, can be enabled at compile time.
                    if (abs(entry->x - x) < winx
                        && entry->value == new_entry->value) {
                        any_duplicate_found = true;
                        outout << new_entry->id << '\t' << entry->id << '\n';
                    }
#else
                    // This is a more optimised version, which breaks out of the loop
                    // on the first match, to work better on files with high duplication
                    // ratio.
                    if (any_duplicate_found == 0
                            && abs(entry->x - x) < winx
                            && entry->value == new_entry->value) {
                        any_duplicate_found = 1;
                        (new_entry)->next = entry;
                        break;
                    }
#endif
                    entry_ptr = &entry->next;
                }
            }
            if (any_duplicate_found) {
                metrics.reads_with_duplicates++;
            }
            metrics.num_reads++;
            *entry_ptr = new_entry;
        }
};

// This returns a result signifying an error.
Metrics error() {
    Metrics m;
    m.error = true;
    return m;
}


class HeaderFormat {
}

HeaderFormat getHeaderFormat(istream& data) {
    string sequence = peek_line(data); // TODO

}

/*
 * Function analysisLoop is called by main program to run the actual analysis.
 *
 * It reads the input file one record at a time, but hands off the parsed 
 * records to an AnalysisHead object.
 *
 * This code is separated into a different function in order to be able to use a 
 * type parameter (VALUE), so it can call the corresponding AnalysisHead efficiently.
 */
template <typename VALUE>
Metrics analysisLoop(
        ostream& output,
        size_t hash_bytes, size_t str_start, size_t str_len,
        int winx, int winy, bool region_sorted, bool unsorted,
        istream& input, const string& header, const string& sequence
        ) {


    // Do some more work to determine the header format. In Illumina format, the 
    // x coordinate is after the fifth colon and the y coordinate after the sixth 
    // colon up to a space.
    size_t start_to_coord_offset = 0, start_to_y_coord_offset = 0;
    size_t colons = 0, end_coords = 0; // temporary variables
    size_t i;
    for (i=0; i<header.size(); ++i) {
        if (header[i] == ':') {
            ++colons;
            if (colons == 5) start_to_coord_offset = i+1;
            if (colons == 6) start_to_y_coord_offset = i+1;
        }
        else if (header[i] == ' ') {
            end_coords = i;
        }
    }
    if (end_coords == 0)
        end_coords = i;
    if (colons < 6) {
        cerr << "Illumina format x/y coordinates not detected" << endl;
        return error();
    }

    // Parse the x/y of the first entry in a special way, because we have already
    // read the lines into memory.
    int x, y;
    x = atoi(header.c_str() + start_to_coord_offset);
    y = atoi(header.c_str() + start_to_y_coord_offset);

    // Group (region) counter, incremented every time the prefix of the read
    // identifier, the string before the x and y coordinates, changes. In
    // unsorted mode, this must be different than 0, as 0 indicates an unknown
    // group.
    int group = 1, unsorted_mode_group_counter = 1;
    map<string, int> unsorted_mode_group;
    int prev_y = 0;

    size_t seq_len = sequence.size();
    if (seq_len >= MAX_LEN) {
        cerr << "Sequence is too long, max supported is: " << MAX_LEN << "." << endl;
        return error();
    }

    // Read identifier prefix (before coordinates)
    char read_id[start_to_coord_offset];
    memcpy(read_id, &header[0], start_to_coord_offset);

    // Sequence buffer
    typename VALUE::SequenceBuffer sequence_buf;
    memset(&sequence_buf, 0, sizeof(sequence_buf));
    // Pointer to read sequence data
    char* linebuf = &sequence_buf.char_data - str_start;
    char* headerbuf = new char[MAX_LEN];
    char* dummybuf = new char[MAX_LEN];
    sequence.copy(&sequence_buf.char_data, str_len, str_start);

    // Need to zero the rest of the string after reading, if it does not fit exactly
    // into the data array. The unused part of data must be zero for TwoBitSequence to
    // work.
    char* zero_start = (&sequence_buf.char_data) + str_len;
    size_t zero_num = sizeof(sequence_buf.data) - str_len;

    // We also accept a header between sequence and quality, doesn't need to be just
    // "+". For example, some files from SRA have this.
    string middle_header;
    getline(input, middle_header);

    if (middle_header.size() == 0 || middle_header[0] != '+') {
        cerr << "The line after the sequence doesn't conform to the expected " 
            << "format." << endl;
        return error();
    }

    input.ignore(1 + seq_len); // Ignores quality scores and \n

    AnalysisHead<VALUE> analysisHead(output, hash_bytes, winx, winy, region_sorted, unsorted);

    cerr << "Started reading FASTQ file..." << endl;

    if (input) {
#ifdef OUTPUT_READ_ID
        analysisHead.enterPoint(group, x, y, header.c_str() + 1, end_coords, sequence_buf.data);
#else
        analysisHead.enterPoint(group, x, y, sequence_buf.data);
#endif
        // This is used only for unordered mode
        unsorted_mode_group[string(header, start_to_coord_offset)] = group;
        while (input) { // Input loop
            input.getline(headerbuf, MAX_LEN);

            if (!input) break;

            // Read the coordinates, then ignore the rest of the header line
            char* ptr;
            x = strtol(headerbuf+start_to_coord_offset, &ptr, 10);
            if (*ptr != ':') {
                cerr << "ERROR: Invalid file format detected. All reads must be of the same length, "
                     << "and the header must be the standard Illumina header." << endl;
                return error();
            }
            y = strtol(ptr+1, &ptr, 10);
            if (*ptr != ' ' && *ptr != '\0') {
                cerr << "ERROR: Invalid file format detected. All reads must be of the same length, "
                     << "and the header must be the standard Illumina header." << endl;
                return error();
            }

            // Read sequence string
            input.getline(linebuf, MAX_LEN);

            // Number of characters read including end of line
            size_t num_read = input.gcount();
            // Ignore the quality header and quality, to the end of the record
            input.getline(dummybuf, MAX_LEN);
            input.getline(dummybuf, MAX_LEN);

            if (!unsorted) {
                // If header prefix doesn't match the last one, signal "end of group" (tile)
                if (memcmp(headerbuf, read_id, start_to_coord_offset) != 0) {
                    group++;
                    memcpy(read_id, headerbuf, start_to_coord_offset);
                    prev_y = 0;
                }
                else if (!region_sorted && y < prev_y) {
                        cerr << "ERROR: The file is not sorted according to y-coordinate. See "
                             << "options --region-sorted or --unsorted." << endl;
                        return error();
                }
            }
            else {
                const string id_str(headerbuf, start_to_coord_offset);
                map<string, int>::iterator location = unsorted_mode_group.find(id_str);
                if (location == unsorted_mode_group.end()) {
                    unsorted_mode_group[id_str] = group = ++unsorted_mode_group_counter; 
                }
                else {
                    group = location->second;
                }
            }
            prev_y = y;

            if (num_read >= 1 + str_len + str_start) {
                for (int bp=0; bp<zero_num; ++bp) zero_start[bp] = 0;
                // Use ID string up to after the y coordinate, so we null-terminate it
#ifdef OUTPUT_READ_ID
                size_t id_len = ptr - headerbuf - 1;
                analysisHead.enterPoint(group, x, y, headerbuf + 1, id_len, sequence_buf.data);
#else
                analysisHead.enterPoint(group, x, y, sequence_buf.data);
#endif
            }

            if (analysisHead.metrics.num_reads % 1000000 == 0)
                cerr << "Analysed " << setw(9) << analysisHead.metrics.num_reads
                    << " reads." << endl;
        }
    }
    return analysisHead.metrics;
}


// -- Main program and housekeeping code below --
// Input paramters, opening I/O streams, etc.

void printUsage(const char* program_name) {
    cerr << "usage: " << program_name << " [options] input_file [output_file] \n";
}

class InputSelector {
    // InputSelector class sets up the input stream from STDIN, or opens a file,
    // and detects whether the input is GZIP compressed.
    private:
        istream* raw_input, *input_ptr;
        unique_ptr<istream> filtered_input;
        ifstream file_input;
        boost::iostreams::filtering_istream in;

    public:
        istream* input;
        bool valid;

        InputSelector(const string& filename) {
            // Disable sync with printf, etc.
            ios_base::sync_with_stdio(false);
            // Use a simple locale, for speed
            setlocale(LC_ALL,"C");
            if (filename == "-") {
                raw_input = &cin;
                // Prevents flushing cout when reading from cin
                cin.tie(nullptr);
            }
            else {
                file_input.open(filename, ios_base::in | ios_base::binary);
                if (file_input.fail()) {
                    valid = false;
                    return;
                }
                raw_input = &file_input;
            }
            
            uint8_t byte1, byte2;

            (*raw_input) >> byte1 >> byte2;

            if ( !raw_input->good() ) {
                valid = false;
                return;
            }
            raw_input->putback(byte2);
            raw_input->putback(byte1);

            if (byte1 == 0x1f && byte2 == 0x8b) {
                in.push(boost::iostreams::gzip_decompressor());
                in.push(*raw_input);
                input = &in;
            }
            else {
                input = raw_input;
            }
            valid = true;
        }
};

int main(int argc, char* argv[]) {

    // Main function: Reads arguments and calls analysisLoop
    
    string inputfile("-"), outputfile("-");
    unsigned int winx, winy;
    int first_base, last_base = -1;
    size_t hash_bytes;
    bool region_sorted, unsorted;

    po::options_description visible("Allowed options");
    visible.add_options()
        ("winx,x", po::value<unsigned int>(&winx)->default_value(2500),
            "x coordinate window, +/- pixels")
        ("winy,y", po::value<unsigned int>(&winy)->default_value(2500),
            "y coordinate window, +/- pixels")
        ("start,s", po::value<int>(&first_base)->default_value(10),
            "First position in reads to consider")
        ("end,e", po::value<int>(&last_base)->default_value(60), 
            "Last position in reads to consider")
        ("region-sorted,r", po::bool_switch(&region_sorted),
            "Assume the input file is sorted by region (tile), but not by (y, x) coordinate "
            "within the region.")
        ("unsorted,u", po::bool_switch(&unsorted),
            "Process unsorted file (a large hash-size is recommended, see --hash-size). This "
            "mode requires all data to be stored in memory, and it is not well optimised.")
        ("hash-size", po::value<size_t>(&hash_bytes)->default_value(512*1024*8), 
            "Hash table size (bytes), must be a power of 2. (increase if winy>2500).")
        ("help,h", "Show this help message")
    ;
    po::options_description positionals("Positional options(hidden)");
    positionals.add_options()
        ("input-file", po::value<string>(&inputfile)->required(),
            "Input file, or - to read from STDIN")
        ("output-file", po::value<string>(&outputfile),
            "Output file, or - to write to STDOUT")
    ;
    po::options_description all_options("Allowed options");
    all_options.add(visible);
    all_options.add(positionals);

    po::positional_options_description pos_desc;
    pos_desc.add("input-file", 1);
    pos_desc.add("output-file", 1);

    po::variables_map vm;
    try {
        po::store(
                po::command_line_parser(argc, argv).options(all_options).positional(pos_desc).run(),
                vm
                );
        if (vm.count("help") > 0) {
            printUsage(argv[0]);
            cerr << visible << '\n';
            cerr << "  Specify - for input_file to read from standard input.\n" << endl;
            return 0;
        }
        else if (vm.count("input-file") != 1) {
            cerr << "ERROR: Argument input_file is required." << endl;
            printUsage(argv[0]);
            cerr << visible << '\n';
            cerr << "  Specify - for input_file to read from stdin." << endl;
            return 1;
        }
        else {
            po::notify(vm);
        }
    }
    catch(po::error& e) 
    { 
      cerr << "ERROR: " << e.what() << "\n\n"; 
      printUsage(argv[0]);
      cerr << visible << endl; 
      return 1; 
    } 

    InputSelector isel(inputfile);

    if (!isel.valid) {
        if (inputfile == "-") {
            cerr << "ERROR: Cannot open standard input: " << strerror(errno) << "\n";
        }
        else {
            cerr << "ERROR: Cannot open file " << inputfile << ": " << strerror(errno) << "\n";
        }
        return 1;
    }

    cerr << "-- suprDUPr v1.0 --\n";

    ostream* output_ptr = &cout;
    ofstream output_file;
    if (vm.count("output-file") > 0) {
        output_file.open(vm["output-file"].as<string>(), ios_base::out);
        output_ptr = &output_file;
    }

    istream&input = *isel.input;
    ostream&output = *output_ptr;

    size_t str_len = (size_t)(last_base - first_base);

    cerr << "Using positions from " << first_base << " to " << first_base+str_len << endl;

    // Call the correct analysis loop for the specified string length
    // All these versions of the analysis loop are compiled as separate 
    // function, but only one is used for a given set of input parameters.
    Metrics result;
    if (str_len > 160) {
        cerr << "ERROR: Sorry, strings longer than 160 characters are not supported "
            << "(use parameters --start, --end)" << endl;
        return 1;
    }
    else if (str_len > 128) {
        result = analysisLoop<TwoBitSequence<5>>(
                output,
                hash_bytes, first_base, str_len, winx, winy, region_sorted, unsorted,
                input, header, sequence
                );
    }
    else if (str_len > 96) {
        result = analysisLoop<TwoBitSequence<4>>(
                output,
                hash_bytes, first_base, str_len, winx, winy, region_sorted, unsorted,
                input, header, sequence
                );
    }
    else if (str_len > 64) {
        result = analysisLoop<TwoBitSequence<3>>(
                output,
                hash_bytes, first_base, str_len, winx, winy, region_sorted, unsorted,
                input, header, sequence
                );
    }
    else if (str_len > 32) {
        result = analysisLoop<TwoBitSequence<2>>(
                output,
                hash_bytes, first_base, str_len, winx, winy, region_sorted, unsorted,
                input, header, sequence
                );
    }
    else if (str_len > 0) {
        result = analysisLoop<TwoBitSequence<1>>(
                output,
                hash_bytes, first_base, str_len, winx, winy, region_sorted, unsorted,
                input, header, sequence
                );
    }
    else {
        cerr << "ERROR: Zero length sequence to compare. Perhaps the format of "
            << "the file was not understood."<< endl;
        return 1;
    }

    if (result.error) {
        return 1; // error flag
    }
    else if (input.eof() && output.good()) {
        cerr << "Completed. Analysed " << result.num_reads << " records." << endl;
#ifdef OUTPUT_READ_ID
        ostream& statsstream = cerr;
#else
        ostream& statsstream = output;
#endif
        statsstream << "NUM_READS\tREADS_WITH_DUP\tDUP_RATIO\n";
        statsstream << result.num_reads 
                    << '\t' << result.reads_with_duplicates 
                    << '\t' << result.reads_with_duplicates * 1.0 / result.num_reads
                    << endl;
        return 0;
    }
    else {
        if (input.bad()) {
            cerr << "ERROR: read: " << strerror(errno) << endl;
        }
        else {
            cerr << "ERROR: Unexpected problem!" << endl;
            cerr << "good=" << input.good() << ", eof=" << input.eof() << endl;
        }
        return 1;
    }
}



