/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2006 Sam Lantinga
    Reparado para GameCube - Solución definitiva de anillo asíncrono anti-estática.
*/
#include "SDL_config.h"

// Public includes.
#include "SDL_timer.h"

// Audio internal includes.
#include "SDL_audio.h"
#include "../SDL_audiomem.h"
#include "../SDL_sysaudio.h"
#include "../SDL_audio_c.h"

// Cube audio internal includes.
#include "SDL_cubeaudio.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <gccore.h>

static const char CUBEAUD_DRIVER_NAME[] = "cube";

// Tamaño del buffer basado en las muestras definidas en SDL_cubeaudio.h
#define DMA_BUFFER_SIZE (SAMPLES_PER_DMA_BUFFER*2*sizeof(short))
#define CUBE_AUDIO_BUFFERS 4

static lwp_t athread = LWP_THREAD_NULL;
static lwpq_t cola = LWP_TQUEUE_NULL;
static CubeAudio *current = NULL;

// ========================================================================
// CONTROL DE ESTADO ASÍNCRONO PARA EL ANILLO DE MEMORIA
// ========================================================================
static volatile int dma_ready_flag[CUBE_AUDIO_BUFFERS] = {0, 0, 0, 0};
static volatile int hardware_active_buffer = 0;

/****************************************************************************
 * Audio Threading (Productor Asíncrono del Mixer de SDL)
 * Llena el anillo de buffers adelantándose al hardware
 ***************************************************************************/
static void *
AudioThread(CubeAudio *private)
{
    u32 buffer_size = DMA_BUFFER_SIZE;
    int search_index = 0;

    LWP_InitQueue(&cola);

    // Precarga inicial: Llenamos los 4 buffers antes de arrancar el hardware para asegurar el colchón
    if (current_audio && !current_audio->paused) {
        int b;
        for (b = 0; b < CUBE_AUDIO_BUFFERS; b++) {
            SDL_LockMutex(current_audio->mixer_lock);
            current_audio->spec.callback(current_audio->spec.userdata, (Uint8 *)(private->dma_buffers[b]), buffer_size);
            SDL_UnlockMutex(current_audio->mixer_lock);

            // Conversión rápida a Big-Endian nativo de GameCube
            u16 *ptr = (u16 *)private->dma_buffers[b];
            int i;
            for(i = 0; i < (buffer_size / 2); i++) {
                ptr[i] = (ptr[i] << 8) | (ptr[i] >> 8);
            }
            DCFlushRange(private->dma_buffers[b], buffer_size);
            dma_ready_flag[b] = 1; // Buffer marcado como listo para el hardware
        }
    }

    while (!private->stopaudio)
    {
        // El hilo se duerme eficientemente hasta que el hardware consuma un bloque
        LWP_ThreadSleep(cola);

        if (private->stopaudio) break;

        if (current_audio && !current_audio->paused)
        {
            // Escaneo circular: Buscamos CUALQUIER buffer que el hardware ya haya vaciado
            int checked;
            for (checked = 0; checked < CUBE_AUDIO_BUFFERS; checked++) {
                
                // Si el buffer está libre (0) y NO es el que el chip de sonido lee en este microsegundo
                if (search_index != hardware_active_buffer && dma_ready_flag[search_index] == 0) {
                    
                    // Bloqueamos el mixer el tiempo mínimo e indispensable para extraer el audio por software
                    SDL_LockMutex(current_audio->mixer_lock);
                    current_audio->spec.callback(current_audio->spec.userdata, (Uint8 *)(private->dma_buffers[search_index]), buffer_size);
                    SDL_UnlockMutex(current_audio->mixer_lock);

                    // Operación rápida de swap de bytes
                    u16 *ptr = (u16 *)private->dma_buffers[search_index];
                    int i;
                    for(i = 0; i < (buffer_size / 2); i++) {
                        ptr[i] = (ptr[i] << 8) | (ptr[i] >> 8);
                    }

                    // Forzar vaciado de caché L1/L2 para coherencia DMA
                    DCFlushRange(private->dma_buffers[search_index], buffer_size);

                    // Levantamos la bandera de disponibilidad
                    dma_ready_flag[search_index] = 1;
                }
                search_index = (search_index + 1) % CUBE_AUDIO_BUFFERS;
            }
        }
    }
    return NULL;
}

/****************************************************************************
 * DMACallback (Consumidor Puro de Hardware)
 ***************************************************************************/
static void
DMACallback(AESNDPB *pb, u32 state)
{
    if (state == VOICE_STATE_STREAM) {
        // 1. El buffer actual terminó de sonar: se libera la bandera de inmediato
        dma_ready_flag[hardware_active_buffer] = 0;

        // 2. Avanzamos de forma circular estricta al siguiente bloque del anillo
        int next_buffer = (hardware_active_buffer + 1) % CUBE_AUDIO_BUFFERS;
        hardware_active_buffer = next_buffer;

        // 3. Encolar el buffer pre-procesado de RAM directamente al hardware mezclador
        AESND_SetVoiceBuffer(pb, current->dma_buffers[hardware_active_buffer], DMA_BUFFER_SIZE);

        // 4. Señal asíncrona: Despierta al AudioThread para rellenar los huecos vacíos de fondo
        LWP_ThreadSignal(cola);
    }
}

void CUBE_AudioStop(CubeAudio *private)
{
    if (private == NULL) {
        if (current == NULL) return;
        private = current;
    }

    private->stopaudio = true;
    if (athread != LWP_THREAD_NULL) {
        LWP_ThreadSignal(cola); 
        LWP_JoinThread(athread, NULL);
        athread = LWP_THREAD_NULL;
    }

    if (private->voice) {
        AESND_SetVoiceStop(private->voice, 1);
        AESND_FreeVoice(private->voice);
        private->voice = NULL;
    }

    AESND_Pause(1);
}

int CUBE_AudioStart(CubeAudio *private)
{
    if (private == NULL) {
        if (current == NULL) return -1;
        private = current;
    }

    memset(private->dma_buffers, 0, sizeof(private->dma_buffers));
    
    // Inicializar limpias las banderas de control del anillo
    int i;
    for (i = 0; i < CUBE_AUDIO_BUFFERS; i++) {
        dma_ready_flag[i] = 0;
    }
    hardware_active_buffer = 0;
    private->stopaudio = false;
    
    current = private; // Asignación requerida antes de alocar la voz por el callback de hardware

    private->voice = AESND_AllocateVoice(DMACallback);
    if (private->voice == NULL) return -1;

    // Hilo en tiempo real con alta prioridad (100) para evitar interferencias del hilo de video
    if (LWP_CreateThread(&athread, (void*(*)(void*))AudioThread, private, private->astack, AUDIOSTACK, 100) < 0) {
        AESND_FreeVoice(private->voice);
        private->voice = NULL;
        return -1;
    }

    AESND_SetVoiceFormat(private->voice, private->format);
    AESND_SetVoiceFrequency(private->voice, private->freq);
    AESND_SetVoiceStream(private->voice, true);
    
    // Arrancamos inyectando el primer buffer pre-cargado
    AESND_SetVoiceBuffer(private->voice, private->dma_buffers[0], DMA_BUFFER_SIZE);
    AESND_SetVoiceStop(private->voice, 0);
    AESND_Pause(0);

    return 1;
}

static int CUBEAUD_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
    u32 format;
    CubeAudio *private = (CubeAudio*)(this->hidden);

    if (spec->freq <= 0 || spec->freq > 144000)
        spec->freq = 48000;

    spec->samples = SAMPLES_PER_DMA_BUFFER;

    switch (spec->format) {
        case AUDIO_S8:
        case AUDIO_U8:
            format = VOICE_MONO8; 
            break;
        default:
        case AUDIO_S16LSB:
        case AUDIO_S16MSB:
            spec->format = AUDIO_S16MSB;
            format = VOICE_MONO16;
            break;
    }

    if (spec->channels > 2) spec->channels = 2;
    if (spec->channels == 2) format++; 

    SDL_CalculateAudioSpec(spec);

    private->format = format;
    private->freq = spec->freq;

    return CUBE_AudioStart(private);
}

static void CUBEAUD_CloseAudio(_THIS)
{
    CUBE_AudioStop((CubeAudio*)(this->hidden));
    current = NULL;
}

static void CUBEAUD_DeleteDevice(_THIS)
{
    CUBE_AudioStop((CubeAudio*)(this->hidden));
    if (this->hidden) free(this->hidden);
    SDL_free(this);
}

static SDL_AudioDevice *CUBEAUD_CreateDevice(int devindex)
{
    SDL_AudioDevice *this;
    athread = LWP_THREAD_NULL;

    this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
    if (this) {
        SDL_memset(this, 0, (sizeof *this));
        // Alineación estricta a 32 bytes en memoria para evitar fallos de caché en GameCube
        this->hidden = (CubeAudio*)memalign(32, sizeof(CubeAudio));
    }

    if ((this == NULL) || (this->hidden == NULL)) {
        if (this) SDL_free(this);
        return NULL;
    }
    SDL_memset(this->hidden, 0, sizeof(CubeAudio));

    AESND_Init();
    AESND_Pause(1);

    this->OpenAudio = CUBEAUD_OpenAudio;
    this->CloseAudio = CUBEAUD_CloseAudio;
    this->free = CUBEAUD_DeleteDevice;

    return this;
}

static int CUBEAUD_Available(void)
{
    return 1;
}

AudioBootStrap CUBEAUD_bootstrap = {
    CUBEAUD_DRIVER_NAME, "SDL cube audio driver",
    CUBEAUD_Available, CUBEAUD_CreateDevice
};