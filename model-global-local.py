# Toy model for determining the relationship between local and global duplicates

import numpy

print("-- Duplicate count toy model --")

# Area parameters
x_lim_tot = (1100, 33000)
y_lim_tot = (1000, 50000)
n_tiles = 112

print("")
print("Coordinate ranges:")
print("  x: ", x_lim_tot)
print("  y: ", y_lim_tot)
print("Number of tiles:", n_tiles)
print("")

# Max distance along the axis (so, the window size is about twice the *_lim_dist)
x_lim_dist = 2500
y_lim_dist = 2500
print("Distance thresholds for local duplicates:")
print("  x: ", x_lim_dist)
print("  y: ", x_lim_dist)
print("")


# Simulate PCR duplicates -- all reads generated from a limited space of original sequences
# nseq_orig sequences amplified up to nseq_reads. The sequences are just represented as 
# integers.
nseq_orig = int(900)
nseq_reads = int(1e3)
print("Number of unique fragments: ", nseq_orig)
print("Number of reads:            ", nseq_reads)
print("")

# Generate nseq_reads by sampling randomly from the space of [0,nseq_orig].
reads = numpy.random.random_integers(1, nseq_orig, (nseq_reads))
xs = numpy.random.random_integers(*x_lim_tot, (nseq_reads))
ys = numpy.random.random_integers(*x_lim_tot, (nseq_reads))
tiles = numpy.random.random_integers(1, n_tiles, (nseq_reads))


# ** Function to compute the duplication ratios, global and local **
def get_duplicate_counts(reads, xs, ys, tiles, x_lim_dist, y_lim_dist):

    # ** Global duplication ratio: count number of duplicates for each unique sequence, regardless
    # of position**
    
    # First get unique sequences (numbers) and the count of each. Only the count is needed for the
    # global duplication ratio.
    
    # We also request return_inverse, which returns an array which is as long as reads, which 
    # contains an index into unique_seqs (array of uniques) for each element in reads. The 
    # variable indexes is used in the next section (local duplicates).
    unique_seqs, indexes, counts = numpy.unique(reads, return_inverse=True, return_counts=True)

    num_global_duplicates = (counts - 1).sum()


    # ** Local duplication ratio **
    # Count the number of reads within a threshold distance, and in the same tile

    local_duplicate_counter = 0

    # Looping over all unique simulated sequences. We only need their index into the
    # unique_seqs array and their count.
    for i, count in ((i, count) for i, count in enumerate(counts) if count != 1):
        # The positions of all duplicates of this read in the arrays reads, xs, ys, tiles
        dup_indexes = numpy.argwhere(indexes == i)

        # Create a matrix with columns tile, y, x, rows for each of the duplicates
        # in this group of sequences
        dup_coords = numpy.concatenate(
            (tiles[dup_indexes],xs[dup_indexes],ys[dup_indexes]),
            axis=1
            )

        for j in range(1, count):
            if numpy.any(
                (dup_coords[j,0] == dup_coords[0:j,0]) &
                (numpy.fabs(dup_coords[j,1] - dup_coords[0:j,1]) < y_lim_dist) &
                (numpy.fabs(dup_coords[j,2] - dup_coords[0:j,2]) < x_lim_dist)
                ):
                local_duplicate_counter += 1

    return num_global_duplicates, local_duplicate_counter

num_global_duplicates, num_local_duplicates = get_duplicate_counts(
            reads, xs, ys, tiles, x_lim_dist, y_lim_dist
        )

print("Global duplication rate: {0:.1f} %".format(num_global_duplicates * 100.0 / nseq_reads))
print("Local duplication rate: {0:.1f} %".format(num_local_duplicates * 100.0 / nseq_reads))
print("")

