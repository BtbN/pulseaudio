/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sink-input.h"
#include "sample-util.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "log.h"

#define CONVERT_BUFFER_LENGTH 4096

struct pa_sink_input* pa_sink_input_new(struct pa_sink *s, const char *name, const struct pa_sample_spec *spec) {
    struct pa_sink_input *i;
    struct pa_resampler *resampler = NULL;
    int r;
    char st[256];
    assert(s && spec);

    if (pa_idxset_ncontents(s->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log(__FILE__": Failed to create sink input: too many inputs per sink.\n");
        return NULL;
    }
    
    if (!pa_sample_spec_equal(spec, &s->sample_spec))
        if (!(resampler = pa_resampler_new(spec, &s->sample_spec, s->core->memblock_stat)))
            return NULL;
    
    i = pa_xmalloc(sizeof(struct pa_sink_input));
    i->name = pa_xstrdup(name);
    i->client = NULL;
    i->owner = NULL;
    i->sink = s;
    i->sample_spec = *spec;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->userdata = NULL;

    i->corked = 0;
    i->volume = PA_VOLUME_NORM;

    i->resampled_chunk.memblock = NULL;
    i->resampled_chunk.index = i->resampled_chunk.length = 0;
    i->resampler = resampler;
    
    assert(s->core);
    r = pa_idxset_put(s->core->sink_inputs, i, &i->index);
    assert(r == 0 && i->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->inputs, i, NULL);
    assert(r == 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log(__FILE__": created %u \"%s\" on %u with sample spec \"%s\"\n", i->index, i->name, s->index, st);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);
    
    return i;    
}

void pa_sink_input_free(struct pa_sink_input* i) {
    assert(i);

    assert(i->sink && i->sink->core);
    pa_idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    pa_idxset_remove_by_data(i->sink->inputs, i, NULL);

    if (i->resampled_chunk.memblock)
        pa_memblock_unref(i->resampled_chunk.memblock);
    if (i->resampler)
        pa_resampler_free(i->resampler);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);
    
    pa_xfree(i->name);
    pa_xfree(i);
}

void pa_sink_input_kill(struct pa_sink_input*i) {
    assert(i);

    if (i->kill)
        i->kill(i);
}

pa_usec_t pa_sink_input_get_latency(struct pa_sink_input *i) {
    pa_usec_t r = 0;
    assert(i);
    
    if (i->get_latency)
        r += i->get_latency(i);

    if (i->resampled_chunk.memblock)
        r += pa_bytes_to_usec(i->resampled_chunk.length, &i->sample_spec);

    return r;
}

int pa_sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    assert(i && chunk && i->peek && i->drop);

    if (i->corked)
        return -1;
    
    if (!i->resampler)
        return i->peek(i, chunk);

    while (!i->resampled_chunk.memblock) {
        struct pa_memchunk tchunk;
        size_t l;
        int ret;
        
        if ((ret = i->peek(i, &tchunk)) < 0)
            return ret;

        assert(tchunk.length);
        
        l = pa_resampler_request(i->resampler, CONVERT_BUFFER_LENGTH);

        if (l > tchunk.length)
            l = tchunk.length;

        i->drop(i, &tchunk, l);
        tchunk.length = l;

        pa_resampler_run(i->resampler, &tchunk, &i->resampled_chunk);
        pa_memblock_unref(tchunk.memblock);
    }

    assert(i->resampled_chunk.memblock && i->resampled_chunk.length);
    *chunk = i->resampled_chunk;
    pa_memblock_ref(i->resampled_chunk.memblock);
    return 0;
}

void pa_sink_input_drop(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length) {
    assert(i && length);

    if (!i->resampler) {
        i->drop(i, chunk, length);
        return;
    }
    
    assert(i->resampled_chunk.memblock && i->resampled_chunk.length >= length);

    i->resampled_chunk.index += length;
    i->resampled_chunk.length -= length;

    if (!i->resampled_chunk.length) {
        pa_memblock_unref(i->resampled_chunk.memblock);
        i->resampled_chunk.memblock = NULL;
        i->resampled_chunk.index = i->resampled_chunk.length = 0;
    }
}

void pa_sink_input_set_volume(struct pa_sink_input *i, pa_volume_t volume) {
    assert(i && i->sink && i->sink->core);

    if (i->volume != volume) {
        i->volume = volume;
        pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    }
}

void pa_sink_input_cork(struct pa_sink_input *i, int b) {
    int n;
    assert(i);
    n = i->corked && !b;
    i->corked = b;

    if (n)
        pa_sink_notify(i->sink);
}
