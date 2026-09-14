# ab_stats.awk - paired statistics over an interleaved A/B result CSV.
#
#   awk -f ab_stats.awk results.csv
#
# Input is `rom,rep,core,fps` with `core` in {baseline, patched}; `#` provenance
# lines and the column header are skipped. Output is tab-separated so ROM names
# containing spaces stay parseable:
#
#   ROM     <name>   <delta_pct>  <t>  <pos>/<n>
#   GROUP   <name>   <mean_delta_pct>  <rom_count>
#
# Deliberately separate from ab_bench.sh so it can be tested without ROMs or a
# built core.
#
# Estimator: the per-ROM figure is ratio-of-means,
# (mean(patched) - mean(baseline)) / mean(baseline). The group figure is the
# unweighted arithmetic mean of the per-ROM deltas, not a geometric mean.
#
# By default every ROM lands in a single "all" group. Pass
# -v synthetic_re='<regex>' to also split the ROMs matching that regex into
# their own "synthetic" group (useful if your own corpus mixes real games
# with diagnostic/test ROMs), leaving everything else under "games".

BEGIN {
    FS = ","
    OFS = "\t"

    ngroups = split("games synthetic all", group_order, " ")
}

/^#/          { next }   # provenance header
NF < 4        { next }
$1 == "rom" && $2 == "rep" { next }   # column header

{
    # ROM names can contain commas, so rebuild the name from everything left of
    # the final three fields rather than trusting $1.
    rom = $1
    for (i = 2; i <= NF - 3; i++)
        rom = rom FS $i
    rep  = $(NF - 2)
    core = $(NF - 1)

    if (!(rom in seen_rom)) {
        seen_rom[rom] = 1
        order[++nrom] = rom
    }
    key = rom SUBSEP rep
    if (!(key in seen_rep)) {
        seen_rep[key] = 1
        reps[rom] = reps[rom] " " rep
    }

    value[rom, rep, core] = $NF + 0
    present[rom, rep, core] = 1
}

END {
    for (r = 1; r <= nrom; r++) {
        rom = order[r]

        n = 0
        usable = 1
        sum_b = 0
        sum_p = 0
        # Explicit " " (not the inherited FS=",") to get awk's whitespace
        # splitting, which also trims the leading separator. The return value
        # is the count: length() over an array is a gawk extension and the awk
        # macOS ships does not have it.
        nreps = split(reps[rom], rep_list, " ")

        for (j = 1; j <= nreps; j++) {
            rp = rep_list[j]
            if (!present[rom, rp, "baseline"] || !present[rom, rp, "patched"]) {
                printf("ab_stats: %s: rep %s has no baseline/patched pair, skipping this ROM\n",
                       rom, rp) > "/dev/stderr"
                usable = 0
                break
            }
            n++
            b = value[rom, rp, "baseline"]
            p = value[rom, rp, "patched"]
            sum_b += b
            sum_p += p
            diff[n] = p - b
        }

        if (!usable || n == 0) {
            skipped++
            continue
        }

        mean_b = sum_b / n
        mean_p = sum_p / n
        delta = (mean_b == 0) ? 0 : (mean_p - mean_b) / mean_b * 100

        mean_d = 0
        for (i = 1; i <= n; i++)
            mean_d += diff[i]
        mean_d /= n

        ss = 0
        for (i = 1; i <= n; i++)
            ss += (diff[i] - mean_d) * (diff[i] - mean_d)
        stdev_d = (n > 1) ? sqrt(ss / (n - 1)) : 0

        positive = 0
        for (i = 1; i <= n; i++)
            if (diff[i] > 0)
                positive++

        # Zero variance is not one case but three. Collapsing them to "inf"
        # reports a nonexistent infinitely-strong result for identical pairs
        # (0/0 is undefined) and drops the sign for a consistent regression.
        # A single pair also lands here: one sample cannot estimate variance.
        if (stdev_d > 0)
            t = sprintf("%.1f", mean_d / (stdev_d / sqrt(n)))
        else if (mean_d > 0)
            t = "+inf"
        else if (mean_d < 0)
            t = "-inf"
        else
            t = "nan"

        printf("ROM\t%s\t%+.1f\t%s\t%d/%d\n", rom, delta, t, positive, n)

        if (synthetic_re != "") {
            group = (rom ~ synthetic_re) ? "synthetic" : "games"
            group_sum[group] += delta
            group_count[group]++
        }
        group_sum["all"] += delta
        group_count["all"]++

        reported++
    }

    for (g = 1; g <= ngroups; g++) {
        name = group_order[g]
        if (group_count[name] > 0)
            printf("GROUP\t%s\t%+.1f\t%d\n",
                   name, group_sum[name] / group_count[name], group_count[name])
    }

    if (reported == 0) {
        print "ab_stats: no usable ROM data in input" > "/dev/stderr"
        exit 2
    }
}
