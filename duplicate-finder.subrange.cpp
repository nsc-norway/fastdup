#include <iostream>
#include <fstream>
#include <memory>
#include <numeric>
#include <algorithm>
#include <string.h>
#include <forward_list>
#include <omp.h>

#include <boost/iostreams/filtering_stream.hpp>
//#include <boost/iostreams/filter/gzip.hpp>
#include "gzip.hpp"
#include <boost/program_options.hpp>

#define BUFFER_SIZE 1024*1024



using namespace std;


// Global flags (sorry)
unsigned int str_len = 50;
unsigned int start_base = 10;


class Entry {
    private:
        Entry(Entry& source) {}
    public:
        bool has_duplicate = false;
        unsigned int duplicates = 0;
        int x;
        const char* seq, * qname;

        // Entry object takes ownership of the string pointers and
        // releases them when destroyed
        Entry(int x, const char* seq, const char* qname) :
            x(x), seq(seq), qname(qname) {
        }

        Entry(Entry&& source) noexcept
           : has_duplicate(source.has_duplicate),
             duplicates(source.duplicates),
             x(source.x), seq(source.seq), qname(source.qname) {
            source.seq = nullptr;
            source.qname = nullptr;
        }

        // No copy constructor!
        Entry(const Entry& source) = delete;

        ~Entry() noexcept {
            delete seq;
            delete qname;
        }

};

typedef vector<Entry> row_data;
class Row {
    public:
        Row(int y, size_t initial_capacity=0) : y(y), done(false) {
            if (initial_capacity > 0) {
                data.reserve(initial_capacity);
            }
        }

        int y;
        bool done;
        row_data data;
};

typedef forward_list<Row*> row_list;


class InputSelector {
    // InputSelector class sets up the input stream based on 
    // command line arguments.
    private:
        istream* raw_input, *input_ptr;
        unique_ptr<istream> filtered_input;
        unique_ptr<char> buffer;
        ifstream file_input;
        boost::iostreams::filtering_istream in;

    public:
        istream* input;

        InputSelector(int argc, char* argv[]) {
            // Use a simple locale, for speed
            setlocale(LC_ALL,"C");
            if (argc == 1 || string(argv[1]) == "-I") {
                raw_input = &cin;
                // Prevents flushing cout when reading from cin
                cin.tie(nullptr);
            }
            else {
                file_input.open(argv[1], ios_base::in | ios_base::binary);
                raw_input = &file_input;
            }
            
            char* buf_array = new char[BUFFER_SIZE];
            raw_input->rdbuf()->pubsetbuf(buf_array, BUFFER_SIZE);
            buffer.reset(buf_array);

            uint8_t byte1, byte2;
            (*raw_input) >> byte1 >> byte2;
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
        }
};

inline int bounded_levenshtein_distance(
    int max_val,
    int s1len, const char* s1,
    int s2len, const char* s2
    )
{
    int buf1[s2len+1], buf2[s2len+1];

    int* column = buf1, * prevcol = buf2;

    for (int i=0; i<s2len+1; i++) {
        prevcol[i] = i;
        column[i] = i;
    }

    int first = 0, last = min(max_val, s2len);
    for (int i = 0; i < s1len; i++) { // Column!
        int j;
        
        if (i >= max_val-1) {
            // First loop: Eliminate leading rows above threshold. Always move one
            // down, so first will be at least incremented by one.
            for (j = first; j < last; j++) { // Row
                // Only need to check diagonally and left, not from top, as it is already 
                // at the max.
                int current_val = min(
                    prevcol[1 + j] + 1,
                    prevcol[j] + (s1[i] == s2[j] ? 0 : 1)
                    );
                if (current_val < max_val) {
                    column[j+1] = current_val;
                    break;
                }
            }
            // Should now point to first row which is below max_val
            first = j+1;
        }
        else {
            column[0] = i+1;
            j = 0;
        }

        // Second loop: find new values in range; keep track of last row below
        // threshold, so we can possibly constrain the "last"
        int last_below = j;
        for (; j <= last && j < s2len; j++) {
            auto possibilities = {
                prevcol[1 + j] + 1,
                column[j] + 1,
                prevcol[j] + (s1[i] == s2[j] ? 0 : 1)
            };
            column[j+1] = std::min(possibilities);
            if (column[j+1] < max_val) {
                last_below = j+1;
            }
        }
        last = last_below;

        swap(column, prevcol);
        if (first > last)
            return max_val;
    }
    return prevcol[s2len];
}



class RowProcessor {

    const size_t seq_len, prefix_len;
    Row& current_row;
    const int y;
    const unsigned int winx, winy;

    public:
        row_list::iterator row_start, row_end;

        RowProcessor(size_t seq_len, size_t prefix_len, Row& current_row,
                unsigned int winx, unsigned int winy,
                row_list::iterator row_start, row_list::iterator row_end)
            : seq_len(str_len), prefix_len(prefix_len),
                current_row(current_row), y(current_row.y),
                winx(winx), winy(winy), row_start(row_start), row_end(row_end) {
        }

        pair<unsigned int, unsigned int> analyseRow() {
            unsigned int total = 0, total_external = 0;

            for (Entry& p0 : current_row.data) {


                // Loop over rows in submatrix
                for (row_list::iterator it_row = row_start; it_row != row_end; ++it_row) {

                    Row& compare_row = **it_row;

                    if ((int)compare_row.y < (int)(y - winy)) {
                        break; // Exit: processed all y in window
                    }

                    // Loop over columns in comparison row
                    int minx = p0.x - winx;
                    int maxx = p0.x + winx;

                    auto itc = compare_row.data.begin();
                    while (itc->x < minx && itc != compare_row.data.end()) {
                        ++itc;
                    }
                    for (; itc != compare_row.data.end(); ++itc) {
                        Entry& pc = *itc;
                        if (pc.x > maxx ) break;

                        // To prevent double-counting, only process up to p0
                        // when comparing row against itself.
                        if (&p0 == &pc) {
                            break;
                        }
                        const int TOO_MANY_DIFFERENCES = 1;
                        // Process pc <--> p0
                        int l = bounded_levenshtein_distance(
                                TOO_MANY_DIFFERENCES,
                                str_len, p0.seq,
                                str_len, pc.seq
                                );
                        bool dup = l < TOO_MANY_DIFFERENCES;
                        
                        if (dup) {
                            #pragma omp critical (output)
                            {
                                //bool p0first = !(y < compare_row.y ||
                                //    (y == compare_row.y && p0.x < pc.x ));
                                //if (p0first) {
                                //   cout.write(p0.qname+1, prefix_len-1);
                                //   cout << p0.x << ':' << y << '_';
                                //}
                                //cout.write(pc.qname+1, prefix_len-1);
                                //cout << pc.x << ':' << compare_row.y;
                                //if (!p0first) {
                                //    cout << '_';
                                //   cout.write(p0.qname+1, prefix_len-1);
                                //   cout << p0.x << ':' << y;
                                //}
                                //cout << '\n';

                                if (pc.duplicates++ == 0) {
                                    total_external++;
                                    // The entry we identified as a duplicate of p0 was not
                                    // already marked as a duplicate.
                                }
                                if (p0.duplicates++ == 0) {
                                   // This is the first duplicate in the current entry
                                   total_external++;
                                }
                                if (!p0.has_duplicate) {
                                    p0.has_duplicate = true;
                                   cout.write(p0.qname+1, prefix_len-1);
                                   cout << p0.x << ':' << y << '\n';
                                    total++;
                                }
                            }
                        }
                    }
                }
            }
            return pair<unsigned int, unsigned int>(total, total_external);
        }
};


class AnalysisHead {

    const size_t seq_len, prefix_len;

    list<row_list*> groups;
    row_list* group;
    Row* current_row;

    unsigned int winx, winy;
    unsigned long total_dups, total_external_dups;
    size_t max_row_size;

    row_list unused_row_cache;

    bool first;

    public:


        AnalysisHead(size_t seq_len, size_t prefix_len,
                unsigned int winx, unsigned int winy) 
            : seq_len(seq_len), prefix_len(prefix_len), winx(winx), winy(winy) {

            total_dups = 0;
            total_external_dups = 0;
            first = true;
            max_row_size = 0;
        }

        ~AnalysisHead() {
            #pragma omp taskwait
            for (auto rowptr : unused_row_cache) {
                delete rowptr;
            }
            list<row_list*>::iterator git;
            for (auto rows_ptr : groups) {
                for (auto rowptr : *rows_ptr) {
                    delete rowptr;
                }
            }
        }

        /* Enter a new sequence. Takes ownership of the string pointers.
         * This should be run inside an omp parallel block, as it may create a task. */
        void enterPoint(int x, int y, const char* seq, const char* qname) {
            if (first) {
                current_row = getRow(y);
                #pragma omp critical(rowOperations)
                {
                    // Here's a new group
                    groups.push_front(new row_list);
                    group = groups.front();
                }
                first = false;
            }
            if (y != current_row->y) {
                max_row_size = max(max_row_size, current_row->data.capacity());
                endOfRow();
                current_row = getRow(y);
            }
            current_row->data.emplace_back(x, seq, qname);
        }

        void endOfGroup() {
            endOfRow();
            #pragma omp critical(rowOperations)
            {
                list<row_list*>::iterator it = groups.begin();
                if (it != groups.end()) ++it; // Skip the first group
                                              // -- won't be finished yet
                while (it != groups.end()) {
                    row_list & rows = **it;
                    bool done = all_of(rows.begin(), rows.end(), [](Row* r) {
                            return r->done;
                        });
                    if (done) {
                        for_each(rows.begin(), rows.end(),
                                [&](Row* item) {unused_row_cache.push_front(item);});
                        delete *it;
                        it = groups.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            first = true;
        }

        pair<unsigned long, unsigned long> getTotal() {
            endOfRow();
            #pragma omp taskwait
            return pair<unsigned long, unsigned long>(total_dups, total_external_dups);
        }

    private:

        Row* getRow(int y) {
            Row* result;
            #pragma omp critical(rowOperations)
            {
                if (!unused_row_cache.empty()) {
                    result = unused_row_cache.front();
                    unused_row_cache.pop_front();
                    result->y = y;
                    result->done = false;
                    result->data.clear();
                }
                else {
                    result = new Row(y, max_row_size);
                }
            }
            return result;
        }

        void endOfRow() {
            row_list * my_group = group;
            Row * my_row = current_row;
            row_list::iterator start, end;

            // Start analysis of a row that was just entered
            #pragma omp critical(rowOperations)
            {
                my_group->push_front(my_row);
                start = my_group->begin();
                end = my_group->end();
            }

            // Pathological scheduling condition when single thread is running:
            // The first rows entered are never cleaned up. Waiting for all tasks
            // removes the last row.
            if (omp_get_num_threads() == 1) {
                #pragma omp taskwait
            }
            
            // Prevent build-up of tasks, instead halt the producer thread
            #pragma omp taskyield
            #pragma omp task firstprivate(my_row, start, end)
            {
                RowProcessor rp(seq_len, prefix_len, *my_row, winx, winy, start, end);
                pair<unsigned int,unsigned int> n_dups = rp.analyseRow();
                #pragma omp atomic
                total_dups += n_dups.first;
                #pragma omp atomic
                total_external_dups += n_dups.second;
                my_row->done = true;
            }
        }

};



int main(int argc, char* argv[]) {

    InputSelector isel(argc, argv);
    istream&input = *isel.input;

    // TODO argument parsing: winx winy start end mismatch


    unsigned int i;
    size_t start_to_coord_offset = 0, coord_to_seq_len = 0;
    string header, sequence;
    getline(input, header);
    getline(input, sequence);
    unsigned int colons = 0;
    size_t end_coords = 0, start_to_y_coord_offset = 0;
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
        cerr << "Invalid format, coordinate not detected" << endl;
        return 1;
    }
    //cerr << "DEBUG I think the end of coordinates is at: " << end_coords << endl;
    //cerr << "DEBUG header size is : " << header.size() << endl;

    int x, y;
    x = atoi(header.c_str() + start_to_coord_offset);
    y = atoi(header.c_str() + start_to_y_coord_offset);
    coord_to_seq_len = header.size() - end_coords + 1;
    size_t i_record = 0, seq_len = sequence.size();

    char* read_name;
    read_name = new char[start_to_coord_offset];
    // Chop of the initial, standard, @ on the header line.
    memcpy(read_name, header.c_str()+1, start_to_coord_offset-1);
    //cerr << "DEBUG the first read-id was " << read_name << endl;

    char* seq_data = new char[str_len];
    sequence.copy(seq_data, str_len, start_base);

    string middle_header;
    getline(input, middle_header);

    if (middle_header.size() == 0 || middle_header[0] != '+') {
        cerr << "The line after the sequence doesn't conform to the expected " 
            << "format." << endl;
        return 1;
    }
    //cerr << "DEBUG middle header " << middle_header << endl;
    //cerr << "DEBUG length of middle header " << middle_header.size() << endl;

    input.ignore(1 + seq_len); // Ignores quality scores

    AnalysisHead analysisHead(seq_len, start_to_coord_offset, 2500, 2500);
    // Enter the first point, we have already read the record. 
    analysisHead.enterPoint(x, y, seq_data, read_name);

    // Set up for OpenMP parallel, but only use one thread for input
    #pragma omp parallel
    #pragma omp single
    {
        char buffer[512], dummy_buffer[512];
        bool valid = true;
        do {
            char* next_read = new char[start_to_coord_offset];
            input.read(next_read, start_to_coord_offset);
            if (input.eof()) break;
            if (memcmp(next_read, read_name, start_to_coord_offset) != 0) {
                analysisHead.endOfGroup();
            }

            // Debug output of read ID
            //cerr << "DEBUG read-id line read:\n";
            //cerr.write(next_read, start_to_coord_offset);
            //cerr << "\n";

            // Input loop
            char colon_test = 0;
            input >> x >> colon_test >> y;
            if (colon_test != ':') {
                cerr << "Input format error (" << colon_test << ")." << endl;
                //cerr << "DEBUG X " << x << " Y " << y << endl;
                valid = false;
                break;
            }

            //cerr << "DEBUG: ignoring " << coord_to_seq_len << endl;
            input.ignore(coord_to_seq_len); 
            input.getline(buffer, 512);
            /*if (next_read[0] != '@') {
             *   cerr << "DEBUG failing at line with " << buffer <<endl;
             *   exit(1);
             *}
             */
            //cerr << "DEBUG: read sequence " << buffer << endl;
            size_t num_read = input.gcount();
            input.getline(dummy_buffer, 512);
            input.getline(dummy_buffer, 512);

            if (num_read >= 1 + str_len + start_base) {
                seq_data = new char[str_len];
                memcpy(seq_data, buffer + start_base, str_len);
                analysisHead.enterPoint(x, y, seq_data, next_read);
            }

            read_name = next_read;
            if (++i_record % 1000000 == 0) {
                cerr << "Read " << i_record << " records (current: ";
                cerr.write(next_read, start_to_coord_offset);
                cerr << x << ':' << y << ")." << endl;
            }
        } while(input && valid);
        if (valid) {
            pair<unsigned int, unsigned int> totals = analysisHead.getTotal();
            cerr << "NUM_READS\tREAD_WITH_DUP\tDUP_RATIO\n";
            cerr << i_record << '\t' << totals.first << '\t' 
                << totals.first * 1.0 / i_record << '\n';
        }
    }

    return 0;
}

