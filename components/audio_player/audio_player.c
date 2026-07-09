/*
 * audio_player.c
 *
 *  Created on: 12.03.2017
 *      Author: michaelboeckling
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"

#include "audio_player.h"
#include "spiram_fifo.h"
#include "freertos/task.h"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_system.h"
#include "esp_log.h"

#include "fdk_aac_decoder.h"
//#include "helix_aac_decoder.h"
//#include "libfaad_decoder.h"
#include "mp3_decoder.h"
#include "webclient.h"
#include "vs1053.h"
#include "app_main.h"

#define TAG "audio_player"
//#define PRIO_MAD configMAX_PRIORITIES - 4


static player_t *player_instance = NULL;
static component_status_t player_status = UNINITIALIZED;

static int start_decoder_task(player_t *player)
{
    TaskFunction_t task_func;
    char * task_name;
    uint16_t stack_depth;
	int priority = PRIO_MAD;

    ESP_LOGD(TAG, "RAM left %d", esp_get_free_heap_size());
	if (get_audio_output_mode() == VS1053)
	{
		task_func = vsTask;
        task_name = (char*)"vsTask";
        stack_depth = 3000;
		priority = PRIO_VS1053;
	} else
    switch (player->media_stream->content_type)
    {
        case AUDIO_MPEG:
            task_func = mp3_decoder_task;
            task_name = (char*)"mp3_decoder_task";
            stack_depth = 8448;
            break;

		case AUDIO_AAC:
        case OCTET_STREAM: // probably .aac
			if (!bigSram())
			{
				ESP_LOGE(TAG, "aac not supported on WROOM cpu");
				spiRamFifoReset();
				clientDisconnect("no AAC");
				return -1;				
			}
		
            task_func = fdkaac_decoder_task;
            task_name = (char*)"fdkaac_decoder_task";
            stack_depth = 6900; //6144; 
            break;

        default:
            ESP_LOGW(TAG, "unknown mime type: %d", player->media_stream->content_type);
			spiRamFifoReset();
            return -1;
    }

	if (((task_func != NULL)) && (xTaskCreatePinnedToCore(task_func, task_name, stack_depth, player,
			priority, NULL, CPU_MAD) != pdPASS)) 
	{
									
		ESP_LOGE(TAG, "ERROR creating decoder task! Out of memory?");
		spiRamFifoReset();
		return -1;
	} else {
		player->decoder_status = RUNNING;
	}
	
	ESP_LOGD(TAG, "decoder task created: %s", task_name);

    return 0;
}

static int t;

/* Writes bytes into the FIFO queue, starts decoder task if necessary. */
int audio_stream_consumer(const char *recv_buf, ssize_t bytes_read)
{

    // don't bother consuming bytes if stopped
    if(player_instance->command == CMD_STOP) {
		clientSilentDisconnect();
        return -2;
    }
	if (bytes_read >0)
		spiRamFifoWrite(recv_buf, bytes_read);

	if (player_instance->decoder_status != RUNNING ) 
	{
//		t = 0;
		int bytes_in_buf = spiRamFifoFill();
		uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();

		//bool buffer_ok = (fill_level > (bigSram()?15:80)); // in %
		if ((fill_level > (bigSram()?15:80)))
		{
			t = 0;
		// buffer is filled, start decoder
			if (start_decoder_task(player_instance) != 0) {
				ESP_LOGE(TAG, "Decoder task failed");
				audio_player_stop();
				clientDisconnect("decoder failed"); 
				return -1;
			}
		}
	}

	if (t == 0) {
		int bytes_in_buf = spiRamFifoFill();
		uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();
		
		ESP_LOGI(TAG, "Buffer fill %u%%, %d // %d bytes", fill_level, bytes_in_buf,spiRamFifoLen());
	}
	t = (t+1) & 255;
	
    return 0;
}

void audio_player_init(player_t *player)
{
    player_instance = player;
    player_status = INITIALIZED;
}

/*
void audio_player_destroy()
{
    if (get_audio_output_mode() != VS1053) renderer_destroy();
    player_status = UNINITIALIZED;
}
*/
void audio_player_start()
{
		if (get_audio_output_mode() != VS1053) renderer_start();
		player_instance->media_stream->eof = false;
		player_instance->command = CMD_START;
		player_instance->decoder_command = CMD_NONE;	
		player_status = RUNNING;
}

void audio_player_stop()
{ 
//		spiRamFifoReset();
		player_instance->decoder_command = CMD_STOP;
		player_instance->command = CMD_STOP;
		player_instance->media_stream->eof = true;
		if (get_audio_output_mode() != VS1053)renderer_stop();

		// FIX: previously this function requested the decoder task to stop
		// (via the flags above) and returned immediately, without waiting
		// for that task to actually finish. The decoder task's cleanup
		// (buf_destroy(in_buf)/buf_destroy(pcm_buf), i2s_driver_uninstall)
		// runs asynchronously in its own FreeRTOS task; if a new station is
		// selected quickly, a NEW decoder task can start - calling
		// i2s_driver_install()/buf_create_dma() - while the OLD task's
		// cleanup is still running concurrently on the same heap/I2S
		// driver. That race is a very plausible cause of the
		// "assert failed: block_trim_free ... block must be free" crash
		// seen right after a station switch, at the exact moment the new
		// decoder task was allocating its DMA buffers. Wait here (bounded,
		// so a stuck decoder task can't hang the whole switch forever)
		// until the old task has really finished before returning.
		if (get_audio_output_mode() != VS1053)
		{
			const TickType_t poll = pdMS_TO_TICKS(10);
			int max_wait_ms = 3000;
			while ((player_instance->decoder_status != STOPPED) && (max_wait_ms > 0))
			{
				vTaskDelay(poll);
				max_wait_ms -= 10;
			}
			if (player_instance->decoder_status != STOPPED)
			{
				ESP_LOGW(TAG, "decoder task did not report STOPPED within timeout, proceeding anyway");
			}
		}

		player_instance->command = CMD_NONE;
		player_status = STOPPED;
}

component_status_t get_player_status()
{
    return player_status;
}

