// SPDX-License-Identifier: GPL-2.0
/*
 * C++ stream style string builder used in KUnit for building messages.
 *
 * Copyright (C) 2018, Google LLC.
 * Author: Brendan Higgins <brendanhiggins@google.com>
 */

#include <linux/list.h>
#include <linux/slab.h>
#include <kunit/string-stream.h>

static int string_stream_vadd(struct string_stream *this,
			       const char *fmt,
			       va_list args)
{
	struct string_stream_fragment *fragment;
	int len;
	va_list args_for_counting;
	unsigned long flags;

	/* Make a copy because `vsnprintf` could change it */
	va_copy(args_for_counting, args);

	/* Need space for null byte. */
	len = vsnprintf(NULL, 0, fmt, args_for_counting) + 1;

	va_end(args_for_counting);

	fragment = kmalloc(sizeof(*fragment), GFP_KERNEL);
	if (!fragment)
		return -ENOMEM;

	fragment->fragment = kmalloc(len, GFP_KERNEL);
	if (!fragment->fragment) {
		kfree(fragment);
		return -ENOMEM;
	}

	len = vsnprintf(fragment->fragment, len, fmt, args);
	spin_lock_irqsave(&this->lock, flags);
	this->length += len;
	list_add_tail(&fragment->node, &this->fragments);
	spin_unlock_irqrestore(&this->lock, flags);
	return 0;
}

int string_stream_add(struct string_stream *this, const char *fmt, ...)
{
	va_list args;
	int result;

	va_start(args, fmt);
	result = string_stream_vadd(this, fmt, args);
	va_end(args);
	return result;
}

void string_stream_clear(struct string_stream *this)
{
	struct string_stream_fragment *fragment, *fragment_safe;
	unsigned long flags;

	spin_lock_irqsave(&this->lock, flags);
	list_for_each_entry_safe(fragment,
				 fragment_safe,
				 &this->fragments,
				 node) {
		list_del(&fragment->node);
		kfree(fragment->fragment);
		kfree(fragment);
	}
	this->length = 0;
	spin_unlock_irqrestore(&this->lock, flags);
}

char *string_stream_get_string(struct string_stream *this)
{
	struct string_stream_fragment *fragment;
	size_t buf_len = this->length + 1; /* +1 for null byte. */
	char *buf;
	unsigned long flags;

	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf)
		return NULL;

	spin_lock_irqsave(&this->lock, flags);
	list_for_each_entry(fragment, &this->fragments, node)
		strlcat(buf, fragment->fragment, buf_len);
	spin_unlock_irqrestore(&this->lock, flags);

	return buf;
}

bool string_stream_is_empty(struct string_stream *this)
{
	bool is_empty;
	unsigned long flags;

	spin_lock_irqsave(&this->lock, flags);
	is_empty = list_empty(&this->fragments);
	spin_unlock_irqrestore(&this->lock, flags);

	return is_empty;
}

void destroy_string_stream(struct string_stream *stream)
{
	stream->clear(stream);
	kfree(stream);
}

static void string_stream_destroy(struct kref *kref)
{
	struct string_stream *stream = container_of(kref,
						    struct string_stream,
						    refcount);
	destroy_string_stream(stream);
}

struct string_stream *new_string_stream(void)
{
	struct string_stream *stream = kzalloc(sizeof(*stream), GFP_KERNEL);

	if (!stream)
		return NULL;

	INIT_LIST_HEAD(&stream->fragments);
	spin_lock_init(&stream->lock);
	kref_init(&stream->refcount);
	stream->add = string_stream_add;
	stream->vadd = string_stream_vadd;
	stream->get_string = string_stream_get_string;
	stream->clear = string_stream_clear;
	stream->is_empty = string_stream_is_empty;
	return stream;
}

void string_stream_get(struct string_stream *stream)
{
	kref_get(&stream->refcount);
}

int string_stream_put(struct string_stream *stream)
{
	return kref_put(&stream->refcount, &string_stream_destroy);
}
