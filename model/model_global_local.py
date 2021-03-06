# Toy model for determining the relationship between local and global duplicates

import numpy
from multiprocessing import Pool, Array, current_process
import ctypes
import os



# Area parameters
x_lim_tot = (1100, 33000)
y_lim_tot = (1000, 50000)
n_tiles = 112

# Max distance along the axis (so, the window size is about twice the *_lim_dist)
x_lim_dist = 2500
y_lim_dist = 2500

# Total reads determines the precision of the simulation. Many parameters are 
# based on total_reads, and will give incorrect results if this is changed.
total_reads = int(300*1e6)

# The strategy is to avoid making "obvious" simplifications, instead simulating 
# everything as it is in real data, to avoid mistakes. There are, however, two (one?)
# major simplifications, done to reduce the runtime:
#  - Integers are used instead of actual nucleotide sequences


# Generate nseq_reads by sampling randomly from the space of [0,nseq_orig].
def generate_reads(nseq_orig, nseq_reads):
    # Simulated reads: There are nseq_orig different possibilities, and nseq_reads
    reads = numpy.random.randint(0, nseq_orig, nseq_reads)
    xs = numpy.random.randint(*x_lim_tot, (nseq_reads))
    ys = numpy.random.randint(*y_lim_tot, (nseq_reads))
    tiles = numpy.random.randint(0, n_tiles, (nseq_reads))
    return reads, xs, ys, tiles


class CounterWorker(object):
    def __init__(self, indexes, tiles, xs, ys, count_list, stride):
        self.indexes, self.tiles, self.xs, self.ys, self.count_list, self.stride\
                = (indexes, tiles, xs, ys, count_list, stride)
    
    def testnode(self, start):
        dup_count = 0
        end = min(self.stride+start, len(self.count_list))
        for ii, icount in self.count_list[start:end]:
            # The positions of all duplicates of this read in the arrays reads, xs, ys, tiles
            dup_indexes = numpy.argwhere(self.indexes == ii)

            # Create a matrix with columns tile, y, x, rows for each of the duplicates
            # in this group of sequences
            dup_coords = numpy.stack(
                (self.tiles[dup_indexes],self.xs[dup_indexes],self.ys[dup_indexes]),
                axis=1
                )

            for j in range(1, icount):
                if numpy.any(
                    (dup_coords[j,0] == dup_coords[0:j,0]) &
                    (numpy.fabs(dup_coords[j,1] - dup_coords[0:j,1]) < y_lim_dist) &
                    (numpy.fabs(dup_coords[j,2] - dup_coords[0:j,2]) < x_lim_dist)
                    ):
                    dup_count += 1
        print(current_process().pid, "> For data starting at", start, "Returning", dup_count)
        return dup_count

global_cw = None
def run_it(start):
    return global_cw.testnode(start)

# ** Function to compute the duplication ratios, global and local **
def get_duplicate_counts(reads, xs, ys, tiles, x_lim_dist, y_lim_dist):
    global global_cw
    # ** Global duplication ratio: count number of duplicates for each unique sequence, regardless
    # of position**
    
    # First get unique sequences (numbers) and the count of each. Only the count is needed for the
    # global duplication ratio.
    
    # We also request return_inverse, which returns an array which is as long as reads, which 
    # contains an index into unique_seqs (array of uniques) for each element in reads. The 
    # variable indexes is used in the next section (local duplicates).
    print(current_process().pid, "> Get unique reads")
    unique_seqs, indexes, counts = numpy.unique(reads, return_inverse=True, return_counts=True)

    print(current_process().pid, "> Compute sum")
    num_global_duplicates = (counts - 1).sum()

    # ** Local duplication ratio **
    # Count the number of reads within a threshold distance, and in the same tile

    # Looping over all unique simulated sequences. We only need their index into the
    # unique_seqs array and their count.
    count_list = numpy.array([(i, count) for i, count in enumerate(counts) if count != 1], 'int64')
    print(current_process().pid, "> Count duplicates, unique entries=", len(count_list))
    stride = 100
    count_list.flags.writeable = False
    global_cw = CounterWorker(indexes, tiles, xs, ys, count_list, stride)
    pool = Pool(len(os.sched_getaffinity(0)))
    joblist = list(range(0, len(count_list), stride))
    print(current_process().pid, "> Number of jobs will be", len(joblist))
    local_duplicate_count = sum(pool.imap_unordered(run_it, joblist))
    #local_duplicate_count = sum(map(run_it, joblist))
    return num_global_duplicates, local_duplicate_count

# Code for analysis of window size
#reads, xs, ys, tiles = generate_reads(10, total_reads) 
#coords = numpy.stack((tiles, ys, xs), axis=1)

def eval_in_range(ctyx):
    tile, y, x = ctyx
    return numpy.sum(
                (coords[:,0] == tile) &
                (numpy.fabs(coords[:,1] - y) < y_lim_dist) &
                (numpy.fabs(coords[:,2] - x) < x_lim_dist)
                )

# Code for main analysis
def analyse(library_size):
    reads, xs, ys, tiles = generate_reads(library_size, total_reads)
    return get_duplicate_counts(reads, xs, ys, tiles, x_lim_dist, y_lim_dist)

