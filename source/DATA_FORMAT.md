# Generated data and plotting guide

All fractions below are in **[0,1]**, not percentages, unless the column explicitly ends in `percent`. Cache capacity uses MiB; footprint uses decimal GB; response times use microseconds. Input trace timestamps and `Avg_interval_ns` use nanoseconds.

## Motivation 1: Page versus Cacheline (and bounded Optimal variants)

For each capacity, `Motivation1/SIZEMB_METRIC.csv` has rows for the six motivation inputs and columns for the selected mechanisms. `*_normalized_to_Page.csv` divides each workload's value by its **ordinary Page** value, never Page-Optimal. A missing/zero denominator yields an empty cell.

Useful metrics:

| Field | Definition |
|---|---|
| `mean_us` | Exact mean of all completed memory-request latencies |
| `hit_ratio` | Overall simulator cache-hit fraction, reads and writes combined |
| `read_hit_ratio`, `write_hit_ratio` | Conditional read/write hit fractions |
| `flash_read_count`, `flush_count` | The simulator's backend-read and flush event counters |
| `io_events_per_request` | (FlashReadCount + FlushCount) / memory requests |
| `io_amplification_4k_equiv` | 4096 × (FlashReadCount + FlushCount) / (64 × requests) |

The last definition intentionally assigns **4 KiB to each counted backend event**, as in the requested accounting convention. It is a page-equivalent measure, not a measurement of actual transferred sector bytes or NAND physical write amplification. More events can coexist with smaller transfers; do not label it physical I/O volume without that qualification. Hit ratio alone does not determine response time.

For the same accounting, `trace_access_GiB` is 64×requests/2^30, `read_4k_equiv_GiB` is 4096×FlashReadCount/2^30, `write_4k_equiv_GiB` is 4096×FlushCount/2^30, and `total_io_4k_equiv_GiB` is their sum. These are GiB, unlike the decimal-GB footprint column.

Plot grouped bars for Page/Cacheline (or all six). Use separate axes/panels for latency and hit ratio, not an unlabeled mixture of units. Workload group labels in the input manifest record the selected study groups; they are not automatic claims about the newly computed outcome.

## Motivation 2: short-term scope distribution and Moran's I

For **every valid trace access t**, let p = floor(addr/4096) and seed = floor((addr mod 4096)/64). Construct the 64-bit bitmap for that page using **the seed plus the next 4096 global memory requests** (reads and writes). Requests to other pages still consume window positions. At trace end, use the remaining requests without padding or discarding the seed.

`A4096(t)` is the popcount of this bitmap. The three access-weighted buckets are Small = 1–4 CL, Medium = 5–16 CL, Large = 17–64 CL; their fractions sum to one. `Mean_active_CL`, median and P90 summarize this per-access population. Coverage is A/64. This is **not** lifetime page coverage, hottest-page coverage or cache-residency coverage.

`Morans_I` uses binary activity x_i on the 64 cachelines, with w_ij=1 for immediate neighbors |i−j|=1 only, and 0 otherwise. No wraparound from CL63 to CL0. In standard notation:

```
I = (64 / 126) * [sum_i sum_j w_ij (x_i-xbar)(x_j-xbar)]
                       / [sum_i (x_i-xbar)^2]
```

Calculate I for each observation, then average the defined values. A fully active page has zero variance: I is undefined, **not 1**. `Undefined_Moran_fraction` reports these excluded observations. The analyzer retains a separately labeled full-as-one diagnostic internally, but the exported Moran column does not use it. Positive adjacency correlation describes clustering, not necessarily a temporally sequential traversal.

`Motivation2/scope_moran_plot_ready.csv` and the workbook's `Motivation2_Scope_Moran` sheet are ready for an Excel combo plot: stack Small/Medium/Large on a primary 0–1 axis, place Moran's I as a marker/line on a separately labeled secondary axis. Mean A is useful additional context, not another stacked component.

## Motivation 3: temporal evolution

`Motivation3/TRACE_coverage_temporal.csv` partitions the entire request sequence into 100 approximately equal-sized bins. Each observation still uses its own sliding future window; bin boundaries do not truncate the 4096 lookahead. Columns include progress (% of requests, not elapsed time), Mean_active_CL, Mean_coverage and their whole-trace means.

Plot `Progress_percent` against `Mean_active_CL`, plus `Trace_mean_active_CL` as a red dashed horizontal line. Alternatively plot both coverage columns. Do not mix CL units and fractions on one unlabeled axis. Whole-trace means are access-weighted, not an unweighted average over unequal final bins.

## Formal experiment and tail means

The same table format covers all nine inputs under `Main_Experiments/`. The workbook includes both absolute and ordinary-Page-normalized tables. Mechanism columns are Page, Cacheline, SkyByte, Nexus, Page-Optimal and Cacheline-Optimal.

Let sorted per-request latency be L[1..n]. `p90_us` is L[ceil(0.90n)] / 1000, and similarly for P95/P99/P99.9/P99.99. By contrast, `tm90_us` is the average of the **largest ceil(0.10n)** samples divided by 1000. TM95 averages the slowest 5%, TM99 the slowest 1%, TM99.9 the slowest 0.1%, TM99.99 the slowest 0.01%. Ties are included by their rank/count, not by including every sample equal to a threshold. These are mixed read/write tails, not an average of separate read and write tails, nor an average across workloads.

## Nexus admission telemetry

Files under `Main_Experiments/Nexus_Admission/` preserve two distinct time series:

- `granularity_timeline`: original million-demand-request bins, actual admission statistics and recent **past-window** policy coverage.
- `granularity_future_alignment`: 100 request-progress bins, comparing per-access **future** A4096 to admission sizes. Both all-access and admission-seed-only A means are retained. No-admission bins have no meaningful admission mean; consumers should mask them using admission_count=0.

`mean_granularity.csv` uses admission_count weights, never averages bin means equally. Distinguish:

- Logical selected CL: the community/admission scope, including already resident CL where applicable.
- Selected CL: population of the selected mask recorded by the implementation; the 1/2/4/... histogram belongs to this field.
- Requested new CL: selected data after resident exclusion at the observation point. It is not guaranteed to equal successfully installed data after replacement/overlap handling.
- Backend sectors: requested 512B-sector units, not 64B CLs.

The future-alignment field `mean_newly_admitted_cl` is the implementation's admission-observer value, not an independently counted final-residency change. Verify the observation point if comparing capacity deltas. Non-power-of-two selected mask counts are recorded as `Other`, not dropped or rounded. Actual `Admission inserted cachelines` counters remain in per-job overall/result records.

Trace A is averaged over **all accesses**, while admission sizes are averaged over **read-miss admission events**. There is no mathematical requirement that either mean be larger or that the two curves coincide. Their phase trends can test whether policy choices respond to locality, but the trace's future observations never drive simulation admission.

## Workload feature table

`selected9_workload_features.csv` uses the full input trace: requests, read fraction, Avg_interval_ns=(last timestamp − first timestamp)/(n−1), and Footprint_GB=(distinct accessed cachelines × 64)/1e9. Footprint is neither the trace file size nor rounded allocated pages. Timestamps are the replay timeline from the supplied translator, **not an unmodified CPU memory issue interval**; see collection documentation for scaling.

HCS is the fraction of all requests directed at each page's hottest 16 CLs: sum(page-local top-16 access counts) / all accesses. RHCS repeats this calculation using reads only and divides by read count. These lifetime concentration indicators are separate from short-term scope and Moran's I; they do not imply a winner. They are generated from the trace, not inferred from simulator outcomes.

## Integrity

The exporter never imports the old paper-results directory. Only matching, completed `result.json` records are eligible. `missing_results.csv` enumerates missing matrix entries, and `completion.json` distinguishes a partial/subset/smoke run from a complete selected-nine run. Simulator overall logs, telemetry, input hashes and manifests are retained for audits.
