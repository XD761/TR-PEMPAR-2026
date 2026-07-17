#!/bin/bash

# 1. Sembunyikan warning 'hwloc' dari OpenMPI agar tidak mengotori layar
export HWLOC_HIDE_ERRORS=1
export OMPI_MCA_btl_base_warn_component_unused=0

FRAMES=600
MAX_CORES=4
PROGRAM="./fire_sim"

echo "BENCHMARK MPI PARTICLE SYSTEM (1 hingga $MAX_CORES Cores) - $FRAMES Frames"
echo "========================================================================"
echo "Proses (p) | Total Waktu (s) | Speedup (S)      | Efisiensi (E)"
echo "------------------------------------------------------------------------"

TIME_BASE=0

for p in $(seq 1 $MAX_CORES); do
    # Catat waktu mulai (dalam detik.nanodetik)
    START=$(date +%s.%N)

    # Jalankan simulasi dengan dummy video agar tidak memunculkan window
    # Output dan error (termasuk sisa warning MPI) dibuang ke /dev/null agar bersih
    # --oversubscribe ditambahkan agar aman jika mencoba core > core fisik komputer
    SDL_VIDEODRIVER=dummy mpirun --oversubscribe -np $p $PROGRAM $FRAMES > /dev/null 2>&1

    # Catat waktu selesai
    END=$(date +%s.%N)

    # Hitung selisih waktu dengan bc (dibagi 1 agar format desimalnya rapi)
    TOTAL_TIME_S=$(echo "scale=4; ($END - $START)/1" | bc)

    # Jika ini iterasi pertama (p=1), simpan sebagai waktu basis (T1)
    if [ $p -eq 1 ]; then
        TIME_BASE=$TOTAL_TIME_S
        SPEEDUP="1.0000"
        EFFICIENCY="100.00"
    else
        # Hitung Speedup (S) = T1 / Tp
        SPEEDUP=$(echo "scale=4; $TIME_BASE / $TOTAL_TIME_S" | bc)
        # Hitung Efisiensi (E) = (S / p) * 100
        EFFICIENCY=$(echo "scale=2; ($SPEEDUP / $p) * 100" | bc)
    fi

    # Cetak hasil ke layar dengan spasi yang rapi seperti format tabel
    printf "%-10s | %-15s | %-16s | %-5s%%\n" "$p" "$TOTAL_TIME_S" "$SPEEDUP" "$EFFICIENCY"
done

echo "========================================================================"
