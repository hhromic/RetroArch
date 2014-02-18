#include "rewind-alcaro.h"
#include <stdlib.h>
#include <string.h>

//format per frame:
//size nextstart;
//repeat {
//  uint16 numchanged; // everything is counted in units of uint16
//  if (numchanged) {
//    uint16 numunchanged; // skip these before handling numchanged
//    uint16[numchanged] changeddata;
//  }
//  else
//  {
//    uint32 numunchanged;
//    if (!numunchanged) break;
//  }
//}
//size thisstart;
//
//the start offsets point to 'nextstart' of any given compressed frame
//multibyte values are stored native endian if alignment is not enforced; if it is, little endian
//the start of the buffer contains a size pointing to the end of the buffer; the end points to its start
//wrapping is handled by returning to the start of the buffer if the compressed data could potentially hit the edge
//if the compressed data could potentially overwrite the tail pointer, the tail retreats until it can no longer collide
//so on average, ~2*maxcompsize is unused at any given moment
//
//if unaligned memory access is illegal, define NO_UNALIGNED_MEM

#if SIZE_MAX == 0xFFFFFFFF
extern char test[(sizeof(size_t)==4)?1:-1];
#elif SIZE_MAX == 0xFFFFFFFFFFFFFFFF
extern char test[(sizeof(size_t)==8)?1:-1];
#define USE_64BIT
#else
#error your compiler is insane.
#endif

#ifdef NO_UNALIGNED_MEM
//These functions assume 16bit alignment.
//They do not make any attempt at matching system native endian; values written by these can only be read by the matching partner.
#ifdef USE_64BIT
static inline void write_size_t(uint16_t* ptr, size_t val)
{
	ptr[0]=val>>0;
	ptr[1]=val>>16;
	ptr[2]=val>>32;
	ptr[3]=val>>48;
}

static inline size_t read_size_t(uint16_t* ptr)
{
	return ((size_t)ptr[0]<<0  |
	        (size_t)ptr[1]<<16 |
	        (size_t)ptr[2]<<32 |
	        (size_t)ptr[3]<<48);
}
#else
static inline void write_size_t(uint16_t* ptr, size_t val)
{
	ptr[0]=val;
	ptr[1]=val>>16;
}

static inline size_t read_size_t(uint16_t* ptr)
{
	return (ptr[0] | (size_t)ptr[1]<<16);
}
#endif

#else
#define read_size_t(ptr) (*(size_t*)(ptr))
#define write_size_t(ptr, val) (*(size_t*)(ptr) = (val))
#endif

struct rewindstack_impl {
	struct rewindstack i;
	
	char * data;
	size_t capacity;
	char * head;//read and write here
	char * tail;//delete here if head is close
	
	char * thisblock;
	char * nextblock;
	bool thisblock_valid;
	
	size_t blocksize;//rounded up from reset::blocksize
	size_t maxcompsize;//size_t+(blocksize+131071)/131072*(blocksize+u16+u16)+u16+u32+size_t
	
	unsigned int entries;
};

static void reset(struct rewindstack * this_, size_t blocksize, size_t capacity)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	
	int newblocksize=((blocksize-1)|(sizeof(uint16_t)-1))+1;
	if (this->blocksize!=newblocksize)
	{
		this->blocksize=newblocksize;
		
		const int maxcblkcover=UINT16_MAX*sizeof(uint16_t);
		const int maxcblks=(this->blocksize+maxcblkcover-1)/maxcblkcover;
		this->maxcompsize=this->blocksize + maxcblks*sizeof(uint16_t)*2 + sizeof(uint16_t)+sizeof(uint32_t) + sizeof(size_t)*2;
		
		free(this->thisblock);
		free(this->nextblock);
		this->thisblock=calloc(this->blocksize+sizeof(uint16_t)*8, 1);
		this->nextblock=calloc(this->blocksize+sizeof(uint16_t)*8, 1);
		//force in a different byte at the end, so we don't need to look for the buffer end in the innermost loop
		//there is also a large amount of data that's the same, to stop the other scan
		//and finally some padding so we don't read outside the buffer end if we're reading in large blocks
		*(uint16_t*)(this->thisblock+this->blocksize+sizeof(uint16_t)*3)=0xFFFF;
		*(uint16_t*)(this->nextblock+this->blocksize+sizeof(uint16_t)*3)=0x0000;
	}
	
	if (capacity!=this->capacity)
	{
		free(this->data);
		this->data=malloc(capacity);
		this->capacity=capacity;
	}
	
	this->head=this->data+sizeof(size_t);
	this->tail=this->data+sizeof(size_t);
	
	this->thisblock_valid=false;
	
	this->entries=0;
}

static void * push_begin(struct rewindstack * this_)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	return this->nextblock;
}

static void push_end(struct rewindstack * this_)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	if (this->thisblock_valid)
	{
		if (this->capacity<sizeof(size_t)+this->maxcompsize) return;
		
	recheckcapacity:;
		size_t headpos=(this->head-this->data);
		size_t tailpos=(this->tail-this->data);
		size_t remaining=(tailpos+this->capacity-sizeof(size_t)-headpos-1)%this->capacity + 1;
		if (remaining<=this->maxcompsize)
		{
			this->tail=this->data + read_size_t((uint16_t*)this->tail);
			this->entries--;
			goto recheckcapacity;
		}
		
		const char* old=this->thisblock;
		const char* new=this->nextblock;
		char* compressed=this->head+sizeof(size_t);
		
		//at the end, 'compressed' must point to the end of the compressed data
		//do not include the next/prev pointers
		//begin compression code
		const uint16_t * old16=(const uint16_t*)old;
		const uint16_t * new16=(const uint16_t*)new;
		uint16_t * compressed16=(uint16_t*)compressed;
		size_t num16s=this->blocksize/sizeof(uint16_t);
		while (num16s)
		{
			const uint16_t * oldprev=old16;
#ifdef NO_UNALIGNED_MEM
			while ((uintptr_t)old16 & (sizeof(size_t)-1) && *old16==*new16)
			{
				old16++;
				new16++;
			}
			if (*old16==*new16)
#endif
			{
				const size_t* olds=(const size_t*)old16;
				const size_t* news=(const size_t*)new16;
				
				while (*olds==*news)
				{
					olds++;
					news++;
				}
				old16=(const uint16_t*)olds;
				new16=(const uint16_t*)news;
				
				while (*old16==*new16)
				{
					old16++;
					new16++;
				}
			}
			size_t skip=(old16-oldprev);
			
			if (skip>=num16s) break;
			num16s-=skip;
			
			if (skip>UINT16_MAX)
			{
				if (skip>UINT32_MAX)
				{
					old16-=skip;
					new16-=skip;
					skip=UINT32_MAX;
					old16+=skip;
					new16+=skip;
				}
				*(compressed16++)=0;
				*(compressed16++)=skip;
				*(compressed16++)=skip>>16;
				compressed16+=2;
				skip=0;
				continue;
			}
			
			size_t changed;
			const uint16_t * old16prev=old16;
			//comparing two or three words makes no real difference
			//with two, the smaller blocks are less likely to be chopped up elsewhere due to 64KB
			//with three, we get larger blocks which should be a minuscle bit faster to decompress, but probably a little slower to compress
			while (old16[0]!=new16[0] || old16[1]!=new16[1])
			{
				old16++;
				new16++;
				while (*old16!=*new16)
				{
					old16++;
					new16++;
				}
			}
			changed=(old16-old16prev);
			if (!changed) continue;
			if (changed>UINT16_MAX)
			{
				old16-=changed;
				new16-=changed;
				changed=UINT16_MAX;
				old16+=changed;
				new16+=changed;
			}
			num16s-=changed;
			*(compressed16++)=changed;
			*(compressed16++)=skip;
			memcpy(compressed16, old16prev, changed*sizeof(uint16_t));
			compressed16+=changed;
		}
		compressed16[0]=0;
		compressed16[1]=0;
		compressed16[2]=0;
		compressed=(char*)(compressed16+3);
		//end compression code
		
		if (compressed-this->data+this->maxcompsize > this->capacity)
		{
			compressed=this->data;
			if (this->tail==this->data+sizeof(size_t)) this->tail=this->data + *(size_t*)this->tail;
		}
		write_size_t((uint16_t*)compressed, this->head-this->data);
		compressed+=sizeof(size_t);
		write_size_t((uint16_t*)this->head, compressed-this->data);
		this->head=compressed;
	}
	else
	{
		this->thisblock_valid=true;
	}
	
	char * swap=this->thisblock;
	this->thisblock=this->nextblock;
	this->nextblock=swap;
	
	this->entries++;
}

static void push_cancel(struct rewindstack * this_)
{
	//struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	//do nothing - push_begin just returns a pointer anyways
}

static const void * pull(struct rewindstack * this_)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	
	if (this->thisblock_valid)
	{
		this->thisblock_valid=false;
		this->entries--;
		return this->thisblock;
	}
	
	if (this->head==this->tail) return NULL;
	
	size_t start=read_size_t((uint16_t*)(this->head - sizeof(size_t)));
	this->head=this->data+start;
	
	const char * compressed=this->data+start+sizeof(size_t);
	char * out=this->thisblock;
	//begin decompression code
	//out is the previously returned state
	const uint16_t * compressed16=(const uint16_t*)compressed;
	uint16_t * out16=(uint16_t*)out;
	while (true)
	{
		uint16_t numchanged=*(compressed16++);
		if (numchanged)
		{
			out16+=*(compressed16++);
			//we could do memcpy, but it seems that function call overhead is high
			// enough that memcpy's higher speed for large blocks won't matter
			for (int i=0;i<numchanged;i++) out16[i]=compressed16[i];
			compressed16+=numchanged;
			out16+=numchanged;
		}
		else
		{
			uint32_t numunchanged=compressed16[0] | compressed16[1]<<16;
			if (!numunchanged) break;
			compressed16+=2;
			out16+=numunchanged;
		}
	}
	//end decompression code
	
	this->entries--;
	
	return this->thisblock;
}

static void capacity_f(struct rewindstack * this_, unsigned int * entries, size_t * bytes, bool * full)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	
	size_t headpos=(this->head-this->data);
	size_t tailpos=(this->tail-this->data);
	size_t remaining=(tailpos+this->capacity-sizeof(size_t)-headpos-1)%this->capacity + 1;
	
	if (entries) *entries=this->entries;
	if (bytes) *bytes=(this->capacity-remaining);
	if (full) *full=(remaining<=this->maxcompsize*2);
}

static void free_(struct rewindstack * this_)
{
	struct rewindstack_impl * this=(struct rewindstack_impl*)this_;
	free(this->data);
	free(this->thisblock);
	free(this->nextblock);
	free(this);
}

struct rewindstack * rewindstack_create(size_t blocksize, size_t capacity)
{
	struct rewindstack_impl * this=malloc(sizeof(struct rewindstack_impl));
	this->i.reset=reset;
	this->i.push_begin=push_begin;
	this->i.push_end=push_end;
	this->i.push_cancel=push_cancel;
	this->i.pull=pull;
	this->i.capacity=capacity_f;
	this->i.free=free_;
	
	this->data=NULL;
	this->thisblock=NULL;
	this->nextblock=NULL;
	
	this->capacity=0;
	this->blocksize=0;
	
	reset((struct rewindstack*)this, blocksize, capacity);
	
	return (struct rewindstack*)this;
}
