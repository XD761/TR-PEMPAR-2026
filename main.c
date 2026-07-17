// Nama : Natanael Kris Setyabudi
// NIM  : 622023018

#include <mpi.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ---------------------- Konfigurasi Simulasi ----------------------
#define WIDTH 800
#define HEIGHT 600

// MODIFIKASI: Jumlah partikel dinaikkan drastis agar padat saat spread full width
#define N_PARTICLES 100000 // dinaikkan agar padat, min 500

#define GRID_COLS 50 // jumlah bucket untuk histogram
#define TARGET_FPS 90
#define FIXED_DT (1.0f / TARGET_FPS)

// MODIFIKASI: Emitor dibuat memenuhi hampir seluruh lebar layar (800) 
#define EMITTER_HALF_WIDTH 300.0f // dinaikkan agar emitor memenuhi lebar 

#define BASE_BUOYANCY 55.0f
#define DENSITY_BUOYANCY_GAIN 6.0f

// MODIFIKASI: Turbulensi dasar dinaikkan agar gerakan api lebih liar 
#define TURBULENCE_STRENGTH 250.0f // dinaikkan untuk efek wavy lebih kuat 

#define VX_DAMPING 0.965f
#define LIFE_MIN 0.9f
#define LIFE_MAX 1.6f

#define MOUSE_REPEL_RADIUS 150.0f // jangkauan pengaruh mouse (px) 
#define MOUSE_REPEL_STRENGTH 1800.0f // kekuatan gaya tolak 

#define FIRE_INTENSITY_DEFAULT 1.0f // skala normal 
#define FIRE_INTENSITY_MIN 0.35f // api paling kecil saat ditekan 'S' 
#define FIRE_INTENSITY_MAX 2.20f // api paling besar saat ditekan 'W' 
#define FIRE_INTENSITY_RATE 0.7f // kecepatan perubahan per detik 

typedef struct {
    float x, y; // posisi 
    float vx, vy; // kecepatan 
    float life; // sisa usia (detik), turun tiap frame 
    float max_life; // usia awal, dipakai untuk hitung fraksi f 
    unsigned int seed; // seed rand_r milik partikel ini 
} Particle;

// Data kontrol interaktif yang hanya diketahui rank 0 (pemilik window),
// lalu disiarkan ke seluruh rank tiap frame lewat satu kali MPI_Bcast. 
typedef struct {
    float mouse_x, mouse_y;
    int mouse_down; // tombol kiri mouse sedang ditahan? 
    int paused; // simulasi sedang dijeda? 
    float intensity; // skala besar/kecil api (W membesar, S mengecil) 
} ControlState;

// Hitung berapa partikel yang menjadi tanggung jawab suatu rank.
// Sisa pembagian (N % size) didistribusikan ke rank-rank pertama
// agar beban kerja serata mungkin. 
static int local_count_for_rank(int rank, int size, int n_total) {
    int base = n_total / size;
    int rem = n_total % size;
    return base + (rank < rem ? 1 : 0);
}

static int local_offset_for_rank(int rank, int size, int n_total) {
    int base = n_total / size;
    int rem = n_total % size;
    int offset = rank * base + (rank < rem ? rank : rem);
    return offset;
}

static void respawn_particle(Particle *p, unsigned int *seed) {
    float jitter = (float)(rand_r(seed) % 2000) / 1000.0f - 1.0f; // -1..1 
    p->x = (float)(WIDTH / 2) + jitter * EMITTER_HALF_WIDTH;
    p->y = (float)(HEIGHT - 2);
    p->vx = ((float)(rand_r(seed) % 2000) / 1000.0f - 1.0f) * 15.0f;
    p->vy = -(60.0f + (float)(rand_r(seed) % 4000) / 1000.0f * 40.0f);
    float lf = LIFE_MIN + (float)(rand_r(seed) % 1000) / 1000.0f * (LIFE_MAX - LIFE_MIN);
    p->life = lf;
    p->max_life = lf;
}

static void init_local_particles(Particle *arr, int count, int global_offset) {
    for (int i = 0; i < count; i++) {
        unsigned int seed = (unsigned int)(global_offset + i) * 2654435761u + 12345u;
        arr[i].seed = seed;
        respawn_particle(&arr[i], &arr[i].seed);
        // sebar usia awal supaya nyala tidak "serempak" saat mulai 
        arr[i].life = ((float)(rand_r(&arr[i].seed) % 1000) / 1000.0f) * arr[i].max_life;
    }
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Warna api berdasarkan fraksi sisa usia f (1 = baru lahir/panas, 0 = padam) 
static void fire_color(float f, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    if (f > 0.80f) { // putih -> kuning pucat 
        float t = (f - 0.80f) / 0.20f;
        *r = 255; *g = 255;
        *b = (Uint8)(180 + 75 * t);
        *a = 255;
    } else if (f > 0.55f) { // kuning -> oranye 
        float t = (f - 0.55f) / 0.25f;
        *r = 255;
        *g = (Uint8)(140 + 90 * t);
        *b = (Uint8)(40 * t);
        *a = 255;
    } else if (f > 0.25f) { // oranye -> merah 
        float t = (f - 0.25f) / 0.30f;
        *r = 200 + (Uint8)(55 * t);
        *g = (Uint8)(60 + 80 * t);
        *b = 10;
        *a = 255;
    } else { // merah -> asap gelap, memudar 
        float t = f / 0.25f;
        *r = (Uint8)(60 * t + 20);
        *g = (Uint8)(60 * t + 20);
        *b = (Uint8)(65 * t + 25);
        *a = (Uint8)(255 * t);
    }
}

int main(int argc, char **argv) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    long max_frames = 0; // 0 = tanpa batas, berhenti hanya lewat window close 
    if (argc > 1) {
        max_frames = strtol(argv[1], NULL, 10);
    }

    // fungsi pembantu
    // ini tugasnya membagi angka N_PARTICLES itu secara adil ke masing-masing prosesor.
    int my_count = local_count_for_rank(rank, size, N_PARTICLES);
    int my_offset = local_offset_for_rank(rank, size, N_PARTICLES);

    // cek di P ke berapa
    Particle *local = (Particle *)malloc(sizeof(Particle) * my_count);
    if (!local) {
        fprintf(stderr, "[rank %d] gagal alokasi memori partikel\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    init_local_particles(local, my_count, my_offset);

    // recvcounts/displs (dalam satuan Particle) hanya dipakai rank 0,
    // tapi setiap rank menghitungnya sendiri (murah, tak perlu komunikasi) 
    int *recvcounts = NULL, *displs = NULL;
    Particle *full = NULL;
    if (rank == 0) {
        recvcounts = (int *)malloc(sizeof(int) * size);
        displs = (int *)malloc(sizeof(int) * size);
        for (int r = 0; r < size; r++) {
            recvcounts[r] = local_count_for_rank(r, size, N_PARTICLES) * (int)sizeof(Particle);
            displs[r] = local_offset_for_rank(r, size, N_PARTICLES) * (int)sizeof(Particle);
        }
        full = (Particle *)malloc(sizeof(Particle) * N_PARTICLES);
    }

    // ---------------- SDL hanya diinisialisasi di rank 0 ---------------- 
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (rank == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init gagal: %s\n", SDL_GetError());
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        window = SDL_CreateWindow("Simulasi Api - MPI Particle System (Full & Wavy)",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
        if (!window) {
            fprintf(stderr, "SDL_CreateWindow gagal: %s\n", SDL_GetError());
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        renderer = SDL_CreateRenderer(window, -1,
                                       SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            // fallback bila vsync/accelerated tidak tersedia (mis. driver dummy) 
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    }

    int quit = 0;
    long frame_no = 0;
    float sim_time = 0.0f;

    ControlState control;
    memset(&control, 0, sizeof(control));
    control.mouse_x = (float)WIDTH / 2.0f;
    control.mouse_y = (float)HEIGHT / 2.0f;
    control.intensity = FIRE_INTENSITY_DEFAULT;

    double t_compute_total = 0.0; // akumulasi waktu update fisika (untuk pengujian) 
    double t_comm_total = 0.0; // akumulasi waktu komunikasi MPI (untuk pengujian) 

    int *local_hist = (int *)malloc(sizeof(int) * GRID_COLS);
    int *global_hist = (int *)malloc(sizeof(int) * GRID_COLS);

    Uint32 last_ticks = (rank == 0) ? SDL_GetTicks() : 0;

    while (!quit) {
        // ---- 1. Rank 0 menangani input/window, siarkan sinyal quit ---- 
        if (rank == 0) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) quit = 1;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
                // e.key.repeat == 0 -> hanya trigger sekali per penekanan,
                // bukan berkali-kali selama tombol ditahan 
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE && e.key.repeat == 0) {
                    control.paused = !control.paused;
                }
            }
            int mx, my;
            Uint32 buttons = SDL_GetMouseState(&mx, &my);
            control.mouse_x = (float)mx;
            control.mouse_y = (float)my;
            control.mouse_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1 : 0;

            // 'W' ditahan -> api membesar; 'S' ditahan -> api mengecil.
            // SDL_GetKeyboardState memberi status "sedang ditekan" tiap
            // frame (bukan event sekali-tekan), pas untuk efek hold. 
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            if (keys[SDL_SCANCODE_W]) control.intensity += FIRE_INTENSITY_RATE * FIXED_DT;
            if (keys[SDL_SCANCODE_S]) control.intensity -= FIRE_INTENSITY_RATE * FIXED_DT;
            if (control.intensity < FIRE_INTENSITY_MIN) control.intensity = FIRE_INTENSITY_MIN;
            if (control.intensity > FIRE_INTENSITY_MAX) control.intensity = FIRE_INTENSITY_MAX;

            char title[128];
            snprintf(title, sizeof(title),
                     "Api | Intensitas: %.2f%s",
                     (double)control.intensity, control.paused ? " [PAUSED]" : "");
            SDL_SetWindowTitle(window, title);
        }
        MPI_Bcast(&quit, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (quit) break;

        if (max_frames > 0 && frame_no >= max_frames) {
            if (rank == 0) quit = 1;
            MPI_Bcast(&quit, 1, MPI_INT, 0, MPI_COMM_WORLD);
            if (quit) break;
        }

        // siarkan posisi mouse, status klik, dan status pause ke semua rank 
        MPI_Bcast(&control, sizeof(ControlState), MPI_BYTE, 0, MPI_COMM_WORLD);

        double t0 = MPI_Wtime();

        // ---- 2. Histogram kepadatan lokal (dari slice sendiri) ---- 
        memset(local_hist, 0, sizeof(int) * GRID_COLS);
        if (!control.paused) {
            for (int i = 0; i < my_count; i++) {
                if (local[i].life > 0.0f) {
                    int col = clampi((int)(local[i].x / (float)WIDTH * GRID_COLS), 0, GRID_COLS - 1);
                    local_hist[col]++;
                }
            }
        }

        double t1 = MPI_Wtime();
        // ---- 3. Pertukaran & penjumlahan histogram ke seluruh rank ---- 
        MPI_Allreduce(local_hist, global_hist, GRID_COLS, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        double t2 = MPI_Wtime();

        // ---- 4. Update fisika partikel pada slice milik rank ini ---- 
        if (!control.paused) {
            for (int i = 0; i < my_count; i++) {
                Particle *p = &local[i];
                p->life -= FIXED_DT;
                if (p->life <= 0.0f || p->y < -20.0f) {
                    respawn_particle(p, &p->seed);
                    // skala intensitas: api "besar" hidup lebih lama & meluncur
                    // lebih kencang ke atas -> nyala tampak lebih tinggi/besar 
                    p->life *= control.intensity;
                    p->max_life *= control.intensity;
                    p->vy *= control.intensity;
                    continue;
                }
                int col = clampi((int)(p->x / (float)WIDTH * GRID_COLS), 0, GRID_COLS - 1);
                float density = (float)global_hist[col];
                float buoyancy = (BASE_BUOYANCY + DENSITY_BUOYANCY_GAIN * (density / 30.0f))
                                 * control.intensity;
                 
                float wavy_force = sinf(sim_time * 4.0f + p->y * 0.01f) * 100.0f * control.intensity;

                // Turbulensi acak dasar (tetap dipertahankan untuk chaos) 
                float basic_turbulence = sinf(sim_time * 3.0f + (float)(p->seed % 1000) * 0.01f) * TURBULENCE_STRENGTH;

                // Gabungkan kedua gaya untuk kecepatan horizontal 
                p->vx = (p->vx + (basic_turbulence + wavy_force) * FIXED_DT) * VX_DAMPING;

                // Perhitungan kecepatan vertikal (buoyancy) tidak diubah drastis 
                p->vy -= buoyancy * FIXED_DT;

                // ---- Efek tolak dari mouse: hanya aktif saat tombol kiri ditahan ---- 
                if (control.mouse_down) {
                    float dx = p->x - control.mouse_x;
                    float dy = p->y - control.mouse_y;
                    float dist2 = dx * dx + dy * dy;
                    if (dist2 < MOUSE_REPEL_RADIUS * MOUSE_REPEL_RADIUS) {
                        float dist = sqrtf(dist2) + 0.001f; // hindari bagi nol tepat di kursor 
                        float falloff = 1.0f - (dist / MOUSE_REPEL_RADIUS); // 1 di pusat, 0 di tepi 
                        float push = falloff * MOUSE_REPEL_STRENGTH;
                        p->vx += (dx / dist) * push * FIXED_DT;
                        p->vy += (dy / dist) * push * FIXED_DT;
                    }
                }

                p->x += p->vx * FIXED_DT;
                p->y += p->vy * FIXED_DT;

                if (p->x < 0.0f) p->x = 0.0f;
                if (p->x > (float)WIDTH) p->x = (float)WIDTH;
            }
        } // end if (!control.paused) 

        double t3 = MPI_Wtime();

        // ---- 5. Kumpulkan seluruh slice ke rank 0 untuk digambar ---- 
        MPI_Gatherv(local, my_count * (int)sizeof(Particle), MPI_BYTE,
                    full, recvcounts, displs, MPI_BYTE,
                    0, MPI_COMM_WORLD);

        double t4 = MPI_Wtime();

        t_compute_total += (t1 - t0) + (t3 - t2);
        t_comm_total += (t2 - t1) + (t4 - t3);

        // ---- 6. Rank 0 menggambar frame ---- 
        if (rank == 0 && max_frames == 0) {
            SDL_SetRenderDrawColor(renderer, 8, 8, 12, 255);
            SDL_RenderClear(renderer);

            for (int i = 0; i < N_PARTICLES; i++) {
                Particle *p = &full[i];
                float f = p->max_life > 0.0f ? (p->life / p->max_life) : 0.0f;
                Uint8 r, g, b, a;
                fire_color(f, &r, &g, &b, &a);
                // ukuran partikel ikut skala intensitas -> api tampak
                // membesar/mengecil, bukan cuma naik lebih tinggi/rendah 
                int sz = (int)((2 + 4.0f * f) * control.intensity);
                if (sz < 1) sz = 1;
                SDL_Rect rect = { (int)p->x - sz / 2, (int)p->y - sz / 2, sz, sz };
                SDL_SetRenderDrawColor(renderer, r, g, b, a);
                SDL_RenderFillRect(renderer, &rect);
            }

            // ---- Indikator bar intensitas di pojok kiri atas ---- 
            {
                const int bar_x = 15, bar_y = 15, bar_w = 18, bar_h = 150;
                float frac = (control.intensity - FIRE_INTENSITY_MIN)
                             / (FIRE_INTENSITY_MAX - FIRE_INTENSITY_MIN);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                int fill_h = (int)(frac * bar_h);

                SDL_SetRenderDrawColor(renderer, 40, 40, 45, 220);
                SDL_Rect bg = { bar_x, bar_y, bar_w, bar_h };
                SDL_RenderFillRect(renderer, &bg);

                SDL_Rect fill = { bar_x, bar_y + (bar_h - fill_h), bar_w, fill_h };
                SDL_SetRenderDrawColor(renderer, 255, 140, 30, 255);
                SDL_RenderFillRect(renderer, &fill);

                SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
                SDL_Rect border = { bar_x, bar_y, bar_w, bar_h };
                SDL_RenderDrawRect(renderer, &border);
            }

            SDL_RenderPresent(renderer);

            // penjagaan framerate manual jika vsync tidak aktif 
            Uint32 now = SDL_GetTicks();
            Uint32 elapsed = now - last_ticks;
            const Uint32 frame_budget = 1000 / TARGET_FPS;
            if (elapsed < frame_budget) SDL_Delay(frame_budget - elapsed);
            last_ticks = SDL_GetTicks();
        }

        if (!control.paused) sim_time += FIXED_DT;
        frame_no++;
    }

    // ---- Ringkasan pengujian performa (opsional, saat max_frames dipakai) ---- 
    if (max_frames > 0) {
        double local_avg_compute_ms = (frame_no > 0) ? (t_compute_total / frame_no) * 1000.0 : 0.0;
        double local_avg_comm_ms = (frame_no > 0) ? (t_comm_total / frame_no) * 1000.0 : 0.0;
        double global_avg_compute_ms = 0.0, global_avg_comm_ms = 0.0;
        MPI_Reduce(&local_avg_compute_ms, &global_avg_compute_ms, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        MPI_Reduce(&local_avg_comm_ms, &global_avg_comm_ms, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        if (rank == 0) {
            printf("[BENCHMARK] proses=%d partikel=%d frame=%ld rata2_compute=%.4fms rata2_komunikasi=%.4fms\n",
                   size, N_PARTICLES, frame_no,
                   global_avg_compute_ms / size, global_avg_comm_ms / size);
        }
    }

    free(local);
    free(local_hist);
    free(global_hist);
    if (rank == 0) {
        free(full);
        free(recvcounts);
        free(displs);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    MPI_Finalize();
    return 0;
}
