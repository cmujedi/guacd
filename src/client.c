
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is guacd.
 *
 * The Initial Developer of the Original Code is
 * Michael Jumper.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#include <guacamole/socket.h>
#include <guacamole/client.h>
#include <guacamole/error.h>
#include <guacamole/protocol.h>
#include <guacamole/timestamp.h>

#include "client.h"
#include "log.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/introspect.h>

#define BUFSIZE 1024

/**
 * Sleep for the given number of milliseconds.
 *
 * @param millis The number of milliseconds to sleep.
 */
void __guacdd_sleep(int millis) {

    struct timespec sleep_period;

    sleep_period.tv_sec =   millis / 1000;
    sleep_period.tv_nsec = (millis % 1000) * 1000000L;

    nanosleep(&sleep_period, NULL);

}

void* __guacd_client_output_thread(void* data) {

    guac_client* client = (guac_client*) data;
    guac_socket* socket = client->socket;

    guac_timestamp last_ping_timestamp = guac_timestamp_current();

    /* Guacamole client output loop */
    while (client->state == GUAC_CLIENT_RUNNING) {

        /* Occasionally ping client with repeat of last sync */
        guac_timestamp timestamp = guac_timestamp_current();
        if (timestamp - last_ping_timestamp > GUACD_SYNC_FREQUENCY) {

            /* Record time of last synnc */
            last_ping_timestamp = timestamp;

            /* Send sync */
            if (guac_protocol_send_sync(socket, client->last_sent_timestamp)) {
                guacd_client_log_guac_error(client,
                        "Error sending \"sync\" instruction");
                guac_client_stop(client);
                return NULL;
            }

            /* Flush */
            if (guac_socket_flush(socket)) {
                guacd_client_log_guac_error(client,
                        "Error flushing output");
                guac_client_stop(client);
                return NULL;
            }

        }

        /* Handle server messages */
        if (client->handle_messages) {

            /* Only handle messages if synced within threshold */
            if (client->last_sent_timestamp - client->last_received_timestamp
                    < GUACD_SYNC_THRESHOLD) {

                int retval = client->handle_messages(client);
                if (retval) {
                    guacd_client_log_guac_error(client,
                            "Error handling server messages");
                    guac_client_stop(client);
                    return NULL;
                }

                /* Send sync instruction */
                client->last_sent_timestamp = guac_timestamp_current();
                if (guac_protocol_send_sync(socket, client->last_sent_timestamp)) {
                    guacd_client_log_guac_error(client, 
                            "Error sending \"sync\" instruction");
                    guac_client_stop(client);
                    return NULL;
                }

                /* Flush */
                if (guac_socket_flush(socket)) {
                    guacd_client_log_guac_error(client,
                            "Error flushing output");
                    guac_client_stop(client);
                    return NULL;
                }

            }

            /* Do not spin while waiting for old sync */
            else
                __guacdd_sleep(GUACD_MESSAGE_HANDLE_FREQUENCY);

        }

        /* If no message handler, just sleep until next sync ping */
        else
            __guacdd_sleep(GUACD_SYNC_FREQUENCY);

    } /* End of output loop */

    guac_client_stop(client);
    return NULL;

}

void* __guacd_client_input_thread(void* data) {

    guac_client* client = (guac_client*) data;
    guac_socket* socket = client->socket;

    /* Guacamole client input loop */
    while (client->state == GUAC_CLIENT_RUNNING) {

        /* Read instruction */
        guac_instruction* instruction =
            guac_instruction_read(socket, GUACD_USEC_TIMEOUT);

        /* Stop on error */
        if (instruction == NULL) {
            guacd_client_log_guac_error(client,
                    "Error reading instruction");
            guac_client_stop(client);
            return NULL;
        }

        /* Reset guac_error and guac_error_message (client handlers are not
         * guaranteed to set these) */
        guac_error = GUAC_STATUS_SUCCESS;
        guac_error_message = NULL;

        /* Call handler, stop on error */
        if (guac_client_handle_instruction(client, instruction) < 0) {

            /* Log error */
            guacd_client_log_guac_error(client,
                    "Client instruction handler error");

            /* Log handler details */
            guac_client_log_info(client,
                    "Failing instruction handler in client was \"%s\"",
                    instruction->opcode);

            guac_instruction_free(instruction);
            guac_client_stop(client);
            return NULL;
        }

        /* Free allocated instruction */
        guac_instruction_free(instruction);

    }

    return NULL;

}

void* __guacd_client_pa_thread(void* data) {

	/* The Sample format to use */
    static const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 44100,
        .channels = 2
    };

    pa_simple *s_in = NULL, *s_out = NULL;
    int error;

    /* This is a command to get the name of the default source 
       which should look like the following:
       alsa_output.pci-0000_00_05.0.analog-stereo.monitor
    */

    FILE *fp;
    char *command = "pactl list | grep -A2 'Source #' | grep 'Name: .*\\.monitor$' | cut -d\" \" -f2";
    char output[100];

    fp = popen(command,"r");

    /* read output from command */
    int result = fscanf(fp,"%s",output);
    if (result == EOF)
        goto finish;

    fclose(fp);

    char *device = output;

    /* Create a new playback stream */

    if (!(s_out = pa_simple_new(NULL, "Playback to default speakers", PA_STREAM_PLAYBACK, NULL, "playback", &ss, NULL, NULL, &error))) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }

    if (!(s_in = pa_simple_new(NULL, "Record from sound card", PA_STREAM_RECORD, device, "record", &ss, NULL, NULL, &error))) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }

    for (;;) {
        uint8_t buf[BUFSIZE];
        pa_usec_t latency;

        if ((latency = pa_simple_get_latency(s_in, &error)) == (pa_usec_t) -1) {
            fprintf(stderr, __FILE__": pa_simple_get_latency() failed: %s\n", pa_strerror(error));
            goto finish;
        }

        if ((latency = pa_simple_get_latency(s_out, &error)) == (pa_usec_t) -1) {
            fprintf(stderr, __FILE__": pa_simple_get_latency() failed: %s\n", pa_strerror(error));
            goto finish;
        }

        if (pa_simple_read(s_in, buf, sizeof(buf), &error) < 0) {

            fprintf(stderr, __FILE__": read() failed: %s\n", strerror(errno));
            goto finish;
        }

        /* ... and play it */
        if (pa_simple_write(s_out, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, __FILE__": pa_simple_write() failed: %s\n", pa_strerror(error));
            goto finish;
        }
    }

    /* Make sure that every single sample was played */
    if (pa_simple_drain(s_out, &error) < 0) {
        fprintf(stderr, __FILE__": pa_simple_drain() failed: %s\n", pa_strerror(error));
        goto finish;
    }

finish:

    if (s_in)
        pa_simple_free(s_in);
    if (s_out)
        pa_simple_free(s_out);

	return NULL;
}

int guacd_client_start(guac_client* client) {

    pthread_t input_thread, output_thread;
	pthread_t pa_thread;

    if (pthread_create(&output_thread, NULL, __guacd_client_output_thread, (void*) client)) {
        guac_client_log_error(client, "Unable to start output thread");
        return -1;
    }

    if (pthread_create(&input_thread, NULL, __guacd_client_input_thread, (void*) client)) {
        guac_client_log_error(client, "Unable to start input thread");
        guac_client_stop(client);
        pthread_join(output_thread, NULL);
        return -1;
    }

 	if (pthread_create(&pa_thread, NULL, __guacd_client_pa_thread, NULL)) {
        guac_client_log_error(client, "Unable to start pulse audio thread");
        guac_client_stop(client);
		pthread_join(input_thread, NULL);        
		pthread_join(output_thread, NULL);		
        return -1;
    }

    /* Wait for I/O threads */
    pthread_join(input_thread, NULL);
    pthread_join(output_thread, NULL);
    pthread_join(pa_thread, NULL);

    /* Done */
    return 0;

}

