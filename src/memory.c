/***************************************************************************************************

Compute Engine - $CE_VERSION_TAG$ <$CE_ID_TAG$>

Copyright (c) 2010, Derek Gerstmann <derek.gerstmann[|AT|]uwa.edu.au> 
The University of Western Australia. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***************************************************************************************************/

#include "memory.h"

/**************************************************************************************************/

typedef struct _ce_session_memory_t {
	ce_memory_info*					host;
	ce_memory_info*					device;
} ce_session_memory_info_t;

typedef struct _ce_memory_block_t
{
	size_t 								size;
	const char* 						filename;
	cl_uint 							line;
	struct _ce_memory_block_t* 			prev;
	struct _ce_memory_block_t* 			next;
} ce_memory_block_t;

typedef struct _ce_memory_info_t 
{
	size_t 								allocations;
	size_t 								deallocations;
	size_t 								max_allowed_bytes;
	size_t 								block_count;
	size_t 								byte_count;
	ce_memory_block_t* 					head_block;
	ce_memory_block_t* 					tail_block;
	cl_bool 							track_sizes;
	size_t 								max_allocated_bytes;
	size_t 								max_block_size;
	size_t 								histogram[32];
} ce_memory_info_t;

/**************************************************************************************************/

static void 
InsertBlock (
	ce_memory_info_t *info, 
	ce_memory_block_t* block)
{
    if (info->tail_block)
    {
        block->prev = info->tail_block;
        block->next = 0;
        info->tail_block->next = block;
        info->tail_block = block;
    }
    else
    {
        block->prev = 0;
        block->next = 0;
        info->head_block = block;
        info->tail_block = block;
    }
}


static void 
RemoveBlock (
	ce_memory_info_t *info, 
	ce_memory_block_t* block)
{
    if (block->prev)
    {
        block->prev->next = block->next;
    }
    else
    {
        info->head_block = block->next;
    }
    
    if (block->next)
    {
        block->next->prev = block->prev;
    }
    else
    {
        info->tail_block = block->prev;
    }
}


/**************************************************************************************************/

void* ceAllocateHostMemory (
	ce_session session,
	size_t bytes, 
	char* filename, 
	unsigned int line)
{
	ce_session_t* s = (ce_session_t*)session;
	ce_session_memory_info_t* memory = (ce_session_memory_info_t*)s->memory;
	ce_memory_info_t* info = memory ? ((ce_memory_info_t*)memory->host) : 0;

	if(!memory || !info)
		return malloc(bytes);

    info->allocations++;
    size_t extended = sizeof(ce_memory_block_t) + bytes;
    char* ptr = (char*)malloc(extended);

    ce_memory_block_t* block = (ce_memory_block_t*)ptr;
    block->size = bytes;
    block->filename = filename;
    block->line = line;
    InsertBlock(info, block);

    ptr += sizeof(ce_memory_block_t);

    info->block_count++;
    info->byte_count += bytes;

    if (info->max_allowed_bytes > 0 && info->byte_count > info->max_allowed_bytes)
    {
        ceWarning(session, "Allocation has exceeded the maximum number of allowed bytes!");
        return 0;
    }

    if (info->byte_count > info->max_allocated_bytes)
    {
        info->max_allocated_bytes = info->byte_count;
    }

    if (info->track_sizes)
    {
        if (bytes > info->max_block_size)
        {
            info->max_block_size = bytes;
        }

        int i;
        unsigned int two_power = 1;
        for (i = 0; i <= 30; i++, two_power <<= 1)
        {
            if (bytes <= two_power)
            {
                info->histogram[i]++;
                break;
            }
        }
        if (i == 31)
        {
            info->histogram[i]++;
        }
    }

    return (void*)ptr;
}


void 
ceDeallocateHostMemory(
	ce_session session, void* ptr)
{
	ce_session_t* s = (ce_session_t*)session;
	ce_session_memory_info_t* memory = (ce_session_memory_info_t*)s->memory;
	ce_memory_info_t* info = memory ? ((ce_memory_info_t*)memory->host) : 0;

	if(!memory || !info)
	{
		free(ptr);
		return;
	}
	
	info->deallocations++;

    if (!ptr)
    {
        return;
    }

    ptr -= sizeof(ce_memory_block_t);

    ce_memory_block_t* block = (ce_memory_block_t*)ptr;
    RemoveBlock(info, block);

	if(info->block_count > 0 && info->byte_count >= block->size)
	{
		info->block_count--;
		info->byte_count -= block->size;
		free(ptr);
	}
	else
	{
        ceWarning(session, "Deallocation size mismatch for memory block!");
		return;
	}
}


void ceLogHostMemoryInfo(
	ce_session session)
{
	ce_session_t* s = (ce_session_t*)session;
	ce_session_memory_info_t* memory = (ce_session_memory_info_t*)s->memory;
	ce_memory_info_t* info = memory ? ((ce_memory_info_t*)memory->host) : 0;

	if(!memory || !info)
	{
		ceWarning(session, "Host memory tracking not enabled!", info->allocations);
		return;
	}
		
	size_t index = 0;
    size_t named_block_count = 0;
    size_t named_byte_count = 0;
    size_t anonymous_block_count = 0;
    size_t anonymous_byte_count = 0;
    ce_memory_block_t* block = 0;

	ceInfo(session, "Total number of host memory allocations: %d", info->allocations);
	ceInfo(session, "Total number of host memory deallocations: %d", info->deallocations);
	ceInfo(session, "Maximum number of bytes allocated in host memory: %d", info->max_allocated_bytes);
	ceInfo(session, "Number of blocks in host memory still allocated: %d", info->block_count);
	ceInfo(session, "Number of bytes in host memory still allocated: %d", info->byte_count);

	block = info->head_block;
    while (block)
    {
        if (block->filename)
        {
            named_block_count++;
            named_byte_count += block->size;
        }
        else
        {
            anonymous_block_count++;
            anonymous_byte_count += block->size;
        }
        block = block->next;
    }

	ceInfo(session, "Number of named blocks in host memory: %d", named_block_count);
	ceInfo(session, "Number of named bytes in host memory: %d", named_byte_count);

	ceInfo(session, "Number of anonymous blocks in host memory: %d", anonymous_block_count);
	ceInfo(session, "Number of anonymous bytes in host memory: %d", anonymous_byte_count);

    block = info->head_block;
    index = 0;
    while (block)
    {

        if (block->filename)
        {
			ceInfo(session, "block[%08d] : %08d bytes -- file: '%s' line '%04d'", index, block->size, block->filename, block->line);
        }
        else
        {
			ceInfo(session, "block[%08d] : %08d bytes -- file: 'unknown' line 'unknown'", index, block->size, block->filename, block->line);
        }
        block = block->next;
        index++;
    }
}