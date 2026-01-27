#!/bin/bash
# Full-system benchmark: compares sensor-w1therm (C) vs sensor-ds18b20 (old)
# Usage: ./run_system_benchmarks.sh [count]

set -e

COUNT=${1:-100}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Output files
NEW_RESULTS="$SCRIPT_DIR/results_w1therm.csv"
OLD_RESULTS="$SCRIPT_DIR/results_ds18b20.csv"
SUMMARY_FILE="$SCRIPT_DIR/system_benchmark_summary.txt"

# Local binary path (new C implementation)
LOCAL_BINARY="$PROJECT_DIR/sensor-w1therm"

# System binaries
NEW_SYSTEM_BINARY="sensor-w1therm"
OLD_SYSTEM_BINARY="sensor-ds18b20"

echo "=============================================="
echo "W1Therm Benchmark: C vs Old Implementation"
echo "=============================================="
echo "Iterations: $COUNT"
echo ""

# Check if local binary exists, build if needed
if [ ! -f "$LOCAL_BINARY" ]; then
    echo "Local binary not found, building..."
    cd "$PROJECT_DIR"
    make
    cd "$SCRIPT_DIR"
    echo "Done."
    echo ""
fi

# Determine which new binary to use (local or system)
if [ -f "$LOCAL_BINARY" ]; then
    NEW_BINARY="$LOCAL_BINARY"
    NEW_LABEL="Local sensor-w1therm (C)"
    echo "Using local binary: $LOCAL_BINARY"
elif command -v "$NEW_SYSTEM_BINARY" &> /dev/null; then
    NEW_BINARY="$(which $NEW_SYSTEM_BINARY)"
    NEW_LABEL="System sensor-w1therm (C)"
    echo "Using system binary: $NEW_BINARY"
else
    echo "ERROR: Could not find sensor-w1therm binary"
    exit 1
fi

# Check if old system binary exists
if command -v "$OLD_SYSTEM_BINARY" &> /dev/null; then
    OLD_BINARY="$(which $OLD_SYSTEM_BINARY)"
    OLD_LABEL="System sensor-ds18b20 (old)"
    echo "Old binary found at: $OLD_BINARY"
    RUN_OLD=1
else
    echo "WARNING: Old binary '$OLD_SYSTEM_BINARY' not found in PATH"
    echo "Only running new implementation benchmark."
    RUN_OLD=0
fi
echo ""

# Clear old result files
rm -f "$NEW_RESULTS" "$OLD_RESULTS" "$SUMMARY_FILE"

# Function to run benchmark on a binary
run_benchmark() {
    local binary_path="$1"
    local output_file="$2"
    local label="$3"
    local count="$4"
    
    echo "==============================================" >&2
    echo "Running $label benchmark ($count iterations)..." >&2
    echo "==============================================" >&2
    
    # Write CSV header
    echo "iteration,exit_code,real_time_sec,user_time_sec,sys_time_sec,output_bytes" > "$output_file"
    
    local successes=0
    local failures=0
    local total_real="0"
    local total_user="0"
    local total_sys="0"
    local total_bytes="0"
    
    for ((i=1; i<=count; i++)); do
        # Create temp files
        local time_file=$(mktemp)
        local output_file_tmp=$(mktemp)
        local exit_code=0
        
        # Run command with timing, capture output size
        /usr/bin/time -f "%e %U %S" -o "$time_file" "$binary_path" > "$output_file_tmp" 2>/dev/null || exit_code=$?
        
        # Get output size
        local output_bytes=$(wc -c < "$output_file_tmp")
        rm -f "$output_file_tmp"
        
        # Parse time output
        if [ -f "$time_file" ] && [ -s "$time_file" ]; then
            read -r real_time user_time sys_time < "$time_file"
        else
            real_time="0"
            user_time="0"
            sys_time="0"
        fi
        rm -f "$time_file"
        
        # Ensure values are valid numbers
        real_time="${real_time:-0}"
        user_time="${user_time:-0}"
        sys_time="${sys_time:-0}"
        output_bytes="${output_bytes:-0}"
        
        # Record result
        echo "$i,$exit_code,$real_time,$user_time,$sys_time,$output_bytes" >> "$output_file"
        
        if [ "$exit_code" -eq 0 ]; then
            ((successes++)) || true
            total_real=$(echo "$total_real + $real_time" | bc)
            total_user=$(echo "$total_user + $user_time" | bc)
            total_sys=$(echo "$total_sys + $sys_time" | bc)
            total_bytes=$((total_bytes + output_bytes))
        else
            ((failures++)) || true
        fi
        
        # Progress indicator every 10 iterations
        if [ $((i % 10)) -eq 0 ]; then
            echo "  Progress: $i/$count (success: $successes, failed: $failures)" >&2
        fi
        
        # Short delay between runs
        sleep 0.1
    done
    
    echo "" >&2
    echo "$label completed: $successes success, $failures failed" >&2
    echo "Results saved to: $output_file" >&2
    echo "" >&2
    
    # Return stats for summary
    echo "$successes $failures $total_real $total_user $total_sys $total_bytes"
}

# Run new (C) binary benchmark
echo ""
NEW_STATS=$(run_benchmark "$NEW_BINARY" "$NEW_RESULTS" "$NEW_LABEL" "$COUNT")
read -r NEW_SUCCESS NEW_FAIL NEW_REAL NEW_USER NEW_SYS NEW_BYTES <<< "$NEW_STATS"

# Run old binary benchmark if available
if [ "$RUN_OLD" -eq 1 ]; then
    echo ""
    OLD_STATS=$(run_benchmark "$OLD_BINARY" "$OLD_RESULTS" "$OLD_LABEL" "$COUNT")
    read -r OLD_SUCCESS OLD_FAIL OLD_REAL OLD_USER OLD_SYS OLD_BYTES <<< "$OLD_STATS"
fi

# Generate summary report
echo "=============================================="
echo "Benchmark Summary"
echo "=============================================="
{
    echo "W1Therm System Benchmark Summary"
    echo "================================"
    echo "Date: $(date)"
    echo "Iterations: $COUNT"
    echo ""
    echo "New Implementation (C): $NEW_BINARY"
    echo "  Success rate: $NEW_SUCCESS/$COUNT ($(echo "scale=1; $NEW_SUCCESS * 100 / $COUNT" | bc)%)"
    if [ "$NEW_SUCCESS" -gt 0 ]; then
        NEW_AVG=$(echo "scale=6; $NEW_REAL / $NEW_SUCCESS" | bc)
        echo "  Avg real time: ${NEW_AVG}s"
        echo "  Avg user time: $(echo "scale=6; $NEW_USER / $NEW_SUCCESS" | bc)s"
        echo "  Avg sys time:  $(echo "scale=6; $NEW_SYS / $NEW_SUCCESS" | bc)s"
        echo "  Total time:    ${NEW_REAL}s"
        echo "  Avg output:    $(echo "scale=0; $NEW_BYTES / $NEW_SUCCESS" | bc) bytes"
    fi
    echo ""
    
    if [ "$RUN_OLD" -eq 1 ]; then
        echo "Old Implementation: $OLD_BINARY"
        echo "  Success rate: $OLD_SUCCESS/$COUNT ($(echo "scale=1; $OLD_SUCCESS * 100 / $COUNT" | bc)%)"
        if [ "$OLD_SUCCESS" -gt 0 ]; then
            OLD_AVG=$(echo "scale=6; $OLD_REAL / $OLD_SUCCESS" | bc)
            echo "  Avg real time: ${OLD_AVG}s"
            echo "  Avg user time: $(echo "scale=6; $OLD_USER / $OLD_SUCCESS" | bc)s"
            echo "  Avg sys time:  $(echo "scale=6; $OLD_SYS / $OLD_SUCCESS" | bc)s"
            echo "  Total time:    ${OLD_REAL}s"
            echo "  Avg output:    $(echo "scale=0; $OLD_BYTES / $OLD_SUCCESS" | bc) bytes"
        fi
        echo ""
        
        # Comparison if both ran successfully
        if [ "$NEW_SUCCESS" -gt 0 ] && [ "$OLD_SUCCESS" -gt 0 ]; then
            echo "Comparison"
            echo "----------"
            
            if [ "$(echo "$NEW_AVG < $OLD_AVG" | bc)" -eq 1 ]; then
                SPEEDUP=$(echo "scale=2; $OLD_AVG / $NEW_AVG" | bc)
                TIME_SAVED=$(echo "scale=4; $OLD_AVG - $NEW_AVG" | bc)
                echo "C implementation is ${SPEEDUP}x faster (saves ${TIME_SAVED}s per read)"
            else
                SPEEDUP=$(echo "scale=2; $NEW_AVG / $OLD_AVG" | bc)
                TIME_SAVED=$(echo "scale=4; $NEW_AVG - $OLD_AVG" | bc)
                echo "Old implementation is ${SPEEDUP}x faster (C is ${TIME_SAVED}s slower)"
            fi
            
            # Calculate time saved over all iterations
            TOTAL_SAVED=$(echo "scale=2; $OLD_REAL - $NEW_REAL" | bc)
            if [ "$(echo "$TOTAL_SAVED > 0" | bc)" -eq 1 ]; then
                echo "Total time saved over $COUNT iterations: ${TOTAL_SAVED}s"
            fi
        fi
    else
        echo "Old Implementation: Not available (sensor-ds18b20 not installed)"
    fi
} | tee "$SUMMARY_FILE"

echo ""
echo "Detailed results saved to:"
echo "  New (C):  $NEW_RESULTS"
if [ "$RUN_OLD" -eq 1 ]; then
    echo "  Old:      $OLD_RESULTS"
fi
echo "  Summary:  $SUMMARY_FILE"
echo ""
echo "Benchmark complete!"
