/* -*- tab-width: 8; c-basic-offset: 4 -*- */

/*
 * File stabs.c - read stabs information from the wine executable itself.
 *
 * Copyright (C) 1996, Eric Youngdale.
 *		 1999, 2000 Eric Pouech
 */

#include "config.h"

#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

#include "debugger.h"

#if defined(__svr4__) || defined(__sun)
#define __ELF__
#endif

#ifdef __ELF__
#ifdef HAVE_ELF_H
# include <elf.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#elif defined(__EMX__)
#ifdef HAVE_A_OUT_H
# include <a_out.h>
#endif
#else
#ifdef HAVE_A_OUT_H
# include <a.out.h>
#endif
#endif

#ifndef N_UNDF
#define N_UNDF		0x00
#endif

#ifndef STN_UNDEF
# define STN_UNDEF	0
#endif

#define N_GSYM		0x20
#define N_FUN		0x24
#define N_STSYM		0x26
#define N_LCSYM		0x28
#define N_MAIN		0x2a
#define N_ROSYM		0x2c
#define N_OPT		0x3c
#define N_RSYM		0x40
#define N_SLINE		0x44
#define N_SO		0x64
#define N_LSYM		0x80
#define N_BINCL		0x82
#define N_SOL		0x84
#define N_PSYM		0xa0
#define N_EINCL		0xa2
#define N_LBRAC		0xc0
#define N_EXCL		0xc2
#define N_RBRAC		0xe0

typedef struct tagELF_DBG_INFO {
    unsigned long	elf_addr;
} ELF_DBG_INFO;

struct stab_nlist {
  union {
    char *n_name;
    struct stab_nlist *n_next;
    long n_strx;
  } n_un;
  unsigned char n_type;
  char n_other;
  short n_desc;
  unsigned long n_value;
};

/*
 * This is used to keep track of known datatypes so that we don't redefine
 * them over and over again.  It sucks up lots of memory otherwise.
 */
struct known_typedef
{
  struct known_typedef * next;
  char		       * name;
  int			 ndefs;
  struct datatype      * types[1];
};

#define NR_STAB_HASH 521

static struct known_typedef * ktd_head[NR_STAB_HASH] = {NULL,};
static struct datatype **     curr_types = NULL;
static int		      allocated_types = 0;

static unsigned int stab_hash( const char * name )
{
    unsigned int hash = 0;
    unsigned int tmp;
    const char * p;

    p = name;

    while (*p) 
      {
	hash = (hash << 4) + *p++;

	if( (tmp = (hash & 0xf0000000)) )
	  {
	    hash ^= tmp >> 24;
	  }
	hash &= ~tmp;
      }
    return hash % NR_STAB_HASH;
}


static void stab_strcpy(char * dest, int sz, const char * source)
{
  /*
   * A strcpy routine that stops when we hit the ':' character.
   * Faster than copying the whole thing, and then nuking the
   * ':'.
   */
  while(*source != '\0' && *source != ':' && sz-- > 0)
      *dest++ = *source++;
  *dest = '\0';
  assert(sz > 0);
}

typedef struct {
   char*		name;
   unsigned long	value;
   int			idx;
   struct datatype**	vector;
   int			nrofentries;
} include_def;

#define MAX_INCLUDES	256

static	include_def* 	include_defs = NULL;
static	int	     	num_include_def = 0;
static  int		num_alloc_include_def = 0;
static	int		cu_include_stack[MAX_INCLUDES];
static	int		cu_include_stk_idx = 0;
static  struct datatype**	cu_vector = NULL;
static  int 		cu_nrofentries = 0;

static 
int	
DEBUG_CreateInclude(const char* file, unsigned long val)
{
  if (num_include_def == num_alloc_include_def) 
    {
      num_alloc_include_def += 256;
      include_defs = DBG_realloc(include_defs, sizeof(include_defs[0])*num_alloc_include_def);
      memset(include_defs+num_include_def, 0, sizeof(include_defs[0])*256);
    }
  include_defs[num_include_def].name = DBG_strdup(file);
  include_defs[num_include_def].value = val;
  include_defs[num_include_def].vector = NULL;
  include_defs[num_include_def].nrofentries = 0;
  
  return num_include_def++;
}

static 
int	
DEBUG_FindInclude(const char* file, unsigned long val)
{
  int		i;
  
  for (i = 0; i < num_include_def; i++) 
    {
      if (val == include_defs[i].value && 
	  strcmp(file, include_defs[i].name) == 0)
	return i;
    }
  return -1;
}

static 
int
DEBUG_AddInclude(int idx)
{
  ++cu_include_stk_idx;
  
  /* is this happen, just bump MAX_INCLUDES */
  /* we could also handle this as another dynarray */
  assert(cu_include_stk_idx < MAX_INCLUDES);
  
  cu_include_stack[cu_include_stk_idx] = idx;
  return cu_include_stk_idx;
}

static
void
DEBUG_ResetIncludes(void)
{
  /*
   * The datatypes that we would need to use are reset when
   * we start a new file. (at least the ones in filenr == 0
   */
  cu_include_stk_idx = 0;/* keep 0 as index for the .c file itself */
  memset(cu_vector, 0, sizeof(cu_vector[0]) * cu_nrofentries);
}

static
void
DEBUG_FreeIncludes(void)
{
  int	i;
  
  DEBUG_ResetIncludes();
  
  for (i = 0; i < num_include_def; i++) 
    {
      DBG_free(include_defs[i].name);
      DBG_free(include_defs[i].vector);
    }
  DBG_free(include_defs);
  include_defs = NULL;
  num_include_def = 0;
  num_alloc_include_def = 0;
  DBG_free(cu_vector);
  cu_vector = NULL;
  cu_nrofentries = 0;
}

static
struct datatype**
DEBUG_FileSubNr2StabEnum(int filenr, int subnr) 
{
  struct datatype** ret;
  
  /* DEBUG_Printf(DBG_CHN_MESG, "creating type id for (%d,%d)\n", filenr, subnr); */
  
  /* FIXME: I could perhaps create a dummy include_def for each compilation
   * unit which would allow not to handle those two cases separately
   */
  if (filenr == 0) 
    {
      if (cu_nrofentries <= subnr) 
	{
	  cu_vector = DBG_realloc(cu_vector, sizeof(cu_vector[0])*(subnr+1));
	  memset(cu_vector+cu_nrofentries, 0, sizeof(cu_vector[0])*(subnr+1-cu_nrofentries));
	  cu_nrofentries = subnr + 1;
	}
      ret = &cu_vector[subnr];
    }
  else
    {
      include_def*	idef;
      
      assert(filenr <= cu_include_stk_idx);
      
      idef = &include_defs[cu_include_stack[filenr]];
      
      if (idef->nrofentries <= subnr)
	{
	  idef->vector = DBG_realloc(idef->vector, sizeof(idef->vector[0])*(subnr+1));
	  memset(idef->vector + idef->nrofentries, 0, sizeof(idef->vector[0])*(subnr+1-idef->nrofentries));
	  idef->nrofentries = subnr + 1;
	}
      ret = &idef->vector[subnr];
    }
  /* DEBUG_Printf(DBG_CHN_MESG,"(%d,%d) is %d\n",filenr,subnr,ret); */
  return ret;
}

static
struct datatype**
DEBUG_ReadTypeEnumBackwards(char*x) {
    int	filenr,subnr;

    if (*x==')') {
	while (*x!='(')
	    x--;
	x++;				/* '(' */
	filenr=strtol(x,&x,10);		/* <int> */
	x++;				/* ',' */
	subnr=strtol(x,&x,10);		/* <int> */
	x++;				/* ')' */
    } else {
	while ((*x>='0') && (*x<='9'))
	    x--;
	filenr = 0;
	subnr = atol(x+1);
    }
    return DEBUG_FileSubNr2StabEnum(filenr,subnr);
}

static 
struct datatype**
DEBUG_ReadTypeEnum(char **x) {
    int filenr,subnr;

    if (**x=='(') {
	(*x)++;					/* '(' */
	filenr=strtol(*x,x,10);			/* <int> */
	(*x)++;					/* ',' */
	subnr=strtol(*x,x,10);			/* <int> */
	(*x)++;					/* ')' */
    } else {
    	filenr = 0;
	subnr = strtol(*x,x,10);		/* <int> */
    }
    return DEBUG_FileSubNr2StabEnum(filenr,subnr);
}

static
int
DEBUG_RegisterTypedef(const char * name, struct datatype ** types, int ndef)
{
  int			 hash;
  struct known_typedef * ktd;

  if( ndef == 1 )
      return TRUE;

  ktd = (struct known_typedef *) DBG_alloc(sizeof(struct known_typedef) 
					 + (ndef - 1) * sizeof(struct datatype *));
  
  hash = stab_hash(name);

  ktd->name = DBG_strdup(name);
  ktd->ndefs = ndef;
  memcpy(&ktd->types[0], types, ndef * sizeof(struct datatype *));
  ktd->next = ktd_head[hash];
  ktd_head[hash] = ktd;

  return TRUE;
}

static
int
DEBUG_HandlePreviousTypedef(const char * name, const char * stab)
{
  int			 count;
  enum debug_type	 expect;
  int			 hash;
  struct known_typedef * ktd;
  char		       * ptr;

  hash = stab_hash(name);

  for(ktd = ktd_head[hash]; ktd; ktd = ktd->next)
      if ((ktd->name[0] == name[0]) && (strcmp(name, ktd->name) == 0) )
	  break;

  /*
   * Didn't find it.  This must be a new one.
   */
  if( ktd == NULL )
      return FALSE;

  /*
   * Examine the stab to make sure it has the same number of definitions.
   */
  count = 0;
  for(ptr = strchr(stab, '='); ptr; ptr = strchr(ptr+1, '='))
    {
      if( count >= ktd->ndefs )
	  return FALSE;

      /*
       * Make sure the types of all of the objects is consistent with
       * what we have already parsed.
       */
      switch(ptr[1])
	{
	case '*':
	  expect = DT_POINTER;
	  break;
	case 's':
	case 'u':
	  expect = DT_STRUCT;
	  break;
	case 'a':
	  expect = DT_ARRAY;
	  break;
	case '(':	/* it's mainly a ref to another typedef, skip it */
          expect = -1;
	  break;
	case '1':
	case 'r':
	  expect = DT_BASIC;
	  break;
	case 'x':
	  expect = DT_STRUCT;
	  break;
	case 'e':
	  expect = DT_ENUM;
	  break;
	case 'f':
	  expect = DT_FUNC;
	  break;
	default:
	  DEBUG_Printf(DBG_CHN_FIXME, "Unknown type (%c).\n",ptr[1]);
	  return FALSE;
	}
      if( expect != -1 && expect != DEBUG_GetType(ktd->types[count]) )
	  return FALSE;
      count++;
    }

  if( ktd->ndefs != count )
      return FALSE;

  /*
   * Go through, dig out all of the type numbers, and substitute the
   * appropriate things.
   */
  count = 0;
  for(ptr = strchr(stab, '='); ptr; ptr = strchr(ptr+1, '='))
      *DEBUG_ReadTypeEnumBackwards(ptr-1) = ktd->types[count++];

  return TRUE;
}

static int DEBUG_FreeRegisteredTypedefs(void)
{
  int			 count;
  int			 j;
  struct known_typedef * ktd;
  struct known_typedef * next;

  count = 0;
  for(j=0; j < NR_STAB_HASH; j++ )
    {
      for(ktd = ktd_head[j]; ktd; ktd = next)
	{
	  count++;
	  next = ktd->next;
	  DBG_free(ktd->name);
	  DBG_free(ktd);
	}  
      ktd_head[j] = NULL;
    }

  return TRUE;

}

static 
int
DEBUG_ParseTypedefStab(char * ptr, const char * typename)
{
  int		    arrmax;
  int		    arrmin;
  char		  * c;
  struct datatype * curr_type;
  struct datatype * datatype;
  char	            element_name[1024];
  int		    ntypes = 0, ntp;
  int		    offset;
  const char	  * orig_typename;
  int		    size;
  char		  * tc;
  char		  * tc2;
  int 		    failure;
  
  orig_typename = typename;

  if( DEBUG_HandlePreviousTypedef(typename, ptr) )
    return TRUE;

  /* 
   * Go from back to front.  First we go through and figure out what
   * type numbers we need, and register those types.  Then we go in
   * and fill the details.  
   */

  for( c = strchr(ptr, '='); c != NULL; c = strchr(c + 1, '=') )
    {
      /*
       * Back up until we get to a non-numeric character, to get datatype
       */
      struct datatype** dt = DEBUG_ReadTypeEnumBackwards(c-1);
      
      if( ntypes >= allocated_types )
	{
	  allocated_types += 64;
	  curr_types = DBG_realloc(curr_types, sizeof(struct datatype*) * allocated_types);
	  if (!curr_types) return FALSE;
	}
      
      switch(c[1])
	{
	case '*':
	  *dt = DEBUG_NewDataType(DT_POINTER, NULL);
	  curr_types[ntypes++] = *dt;
	  break;
	case 's':
	case 'u':
	  *dt = DEBUG_NewDataType(DT_STRUCT, typename);
	  curr_types[ntypes++] = *dt;
	  break;
	case 'a':
	  *dt = DEBUG_NewDataType(DT_ARRAY, NULL);
	  curr_types[ntypes++] = *dt;
	  break;
	case '(':
	  /* will be handled in next loop, 
	   * just a ref to another type 
	   */
	  curr_types[ntypes++] = NULL;
	  break;
	case '1':
	case 'r':
	  *dt = DEBUG_NewDataType(DT_BASIC, typename);
	  curr_types[ntypes++] = *dt;
	  break;
	case 'x':
	  stab_strcpy(element_name, sizeof(element_name), c + 3);
	  *dt = DEBUG_NewDataType(DT_STRUCT, element_name);
	  curr_types[ntypes++] = *dt;
	  break;
	case 'e':
	  *dt = DEBUG_NewDataType(DT_ENUM, NULL);
	  curr_types[ntypes++] = *dt;
	  break;
	case 'f':
	  *dt = DEBUG_NewDataType(DT_FUNC, NULL);
	  curr_types[ntypes++] = *dt;
	  break;
	default:
	  DEBUG_Printf(DBG_CHN_FIXME, "Unknown type (%c).\n",c[1]);
	  return FALSE;
	}
      typename = NULL;
      
    }

  ntp = ntypes - 1;
  /* 
   * OK, now take a second sweep through.  Now we will be digging
   * out the definitions of the various components, and storing
   * them in the skeletons that we have already allocated.  We take
   * a right-to left search as this is much easier to parse.  
   */
  for( c = strrchr(ptr, '='); c != NULL; c = strrchr(ptr, '=') )
    {
      struct datatype** dt = DEBUG_ReadTypeEnumBackwards(c-1);
      struct datatype** dt2;
	
      curr_type = *dt;
      
      switch(c[1])
	{
	case 'x':
	  ntp--;
	  tc = c + 3;
	  while( *tc != ':' )
	    tc++;
	  tc++;
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc);
	  break;
	case '*':
	case 'f':
	  ntp--;
	  tc = c + 2;
	  datatype = *DEBUG_ReadTypeEnum(&tc);
	  DEBUG_SetPointerType(curr_type, datatype);
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc);
	  break;
	case '(':
	  tc = c + 1;
	  dt2 = DEBUG_ReadTypeEnum(&tc);
	  
	  if (!*dt && *dt2) 
	    {
	      *dt = *dt2;
	    } 
	  else if (!*dt && !*dt2) 
	    {
	      /* this should be a basic type, define it */
	      *dt2 = *dt = DEBUG_NewDataType(DT_BASIC, typename);
	    } 
	  else 
	    {
	      DEBUG_Printf(DBG_CHN_MESG, "Unknown condition %08lx %08lx (%s)\n", 
			   (unsigned long)*dt, (unsigned long)*dt2, ptr);
	    }
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc);
	  curr_types[ntp--] = *dt;
	  break;
	case '1':
	case 'r':
	  ntp--;
	  /*
	   * We have already handled these above.
	   */
	  *c = '\0';
	  break;
	case 'a':
	  ntp--;
	  /* ar<typeinfo_nodef>;<int>;<int>;<typeinfo>,<int>,<int>;; */
	  
	  tc  = c + 3;
	  /* 'r' */
	  DEBUG_ReadTypeEnum(&tc);
	  tc++;		 			/* ';' */
	  arrmin = strtol(tc, &tc, 10); 	/* <int> */
	  tc++;		 			/* ';' */
	  arrmax = strtol(tc, &tc, 10);		/* <int> */
	  tc++;		 			/* ';' */
	  datatype = *DEBUG_ReadTypeEnum(&tc);  /* <typeinfo> */
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc);
	  DEBUG_SetArrayParams(curr_type, arrmin, arrmax, datatype);
	  break;
	case 's':
	case 'u':
	  ntp--;
	  failure = 0;
	  
	  tc = c + 2;
	  if( DEBUG_SetStructSize(curr_type, strtol(tc, &tc, 10)) == FALSE )
	    {
	      /*
	       * We have already filled out this structure.  Nothing to do,
	       * so just skip forward to the end of the definition.
	       */
	      while( tc[0] != ';' && tc[1] != ';' )
		tc++;
	      
	      tc += 2;
	      
	      if( *tc == '\0' )
		*c = '\0';
	      else
		strcpy(c, tc + 1);
	      continue;
	    }
	  
	  /*
	   * Now parse the individual elements of the structure/union.
	   */
	  while(*tc != ';')
	    {
	      char *ti;
	      tc2 = element_name;
	      while(*tc != ':')
		*tc2++ = *tc++;
	      tc++;
	      *tc2++ = '\0';
	      ti=tc;
	      datatype = *DEBUG_ReadTypeEnum(&tc);
	      *tc='\0';
	      tc++;
	      offset  = strtol(tc, &tc, 10);
	      tc++;
	      size  = strtol(tc, &tc, 10);
	      tc++;
	      if (datatype)
		DEBUG_AddStructElement(curr_type, element_name, datatype, 
				       offset, size);
	      else 
		{
		  failure = 1;
		  /* ... but proceed parsing to the end of the stab */
		  DEBUG_Printf(DBG_CHN_MESG, "failure on %s %s\n", ptr, ti);
		}
	    }
	  
	  if (failure) 
	    {
	      
	      /* if we had a undeclared value this one is undeclared too.
	       * remove it from the stab_types. 
	       * I just set it to NULL to detect bugs in my thoughtprocess.
	       * FIXME: leaks the memory for the structure elements.
	       * FIXME: such structures should have been optimized away
	       *        by ld.
	       */
	      *dt = NULL;
	    }
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc + 1);
	  break;
	case 'e':
	  ntp--;
	  tc = c + 2;
	  /*
	   * Now parse the individual elements of the structure/union.
	   */
	  while(*tc != ';')
	    {
	      tc2 = element_name;
	      while(*tc != ':')
		*tc2++ = *tc++;
	      tc++;
	      *tc2++ = '\0';
	      offset  = strtol(tc, &tc, 10);
	      tc++;
	      DEBUG_AddStructElement(curr_type, element_name, NULL, offset, 0);
	    }
	  if( *tc == '\0' )
	    *c = '\0';
	  else
	    strcpy(c, tc + 1);
	  break;
	default:
	  DEBUG_Printf(DBG_CHN_FIXME, "Unknown type (%c).\n",c[1]);
	  return FALSE;
	}
    }
  /*
   * Now register the type so that if we encounter it again, we will know
   * what to do.
   */
  DEBUG_RegisterTypedef(orig_typename, curr_types, ntypes);
    
  return TRUE;
}

static struct datatype *
DEBUG_ParseStabType(const char * stab)
{
  char * c;

  /*
   * Look through the stab definition, and figure out what datatype
   * this represents.  If we have something we know about, assign the
   * type.
   */
  c = strchr(stab, ':');
  if( c == NULL )
      return NULL;

  c++;
  /*
   * The next character says more about the type (i.e. data, function, etc)
   * of symbol.  Skip it.
   */
  if (*c != '(')
    c++;
  /* 
   * The next is either an integer or a (integer,integer).
   * The DEBUG_ReadTypeEnum takes care that stab_types is large enough.
   */
  return *DEBUG_ReadTypeEnum(&c);
}

enum DbgInfoLoad DEBUG_ParseStabs(char * addr, unsigned int load_offset,
				  unsigned int staboff, int stablen, 
				  unsigned int strtaboff, int strtablen)
{
  struct name_hash    * curr_func = NULL;
  struct wine_locals  * curr_loc = NULL;
  struct name_hash    * curr_sym = NULL;
  char                  currpath[PATH_MAX];
  int                   i;
  int                   in_external_file = FALSE;
  int                   last_nso = -1;
  int                   len;
  DBG_VALUE	        new_value;
  int                   nstab;
  char                * ptr;
  char                * stabbuff;
  int                   stabbufflen;
  struct stab_nlist   * stab_ptr;
  char                * strs;
  int                   strtabinc;
  char                * subpath = NULL;
  char                  symname[4096];

  nstab = stablen / sizeof(struct stab_nlist);
  stab_ptr = (struct stab_nlist *) (addr + staboff);
  strs  = (char *) (addr + strtaboff);

  memset(currpath, 0, sizeof(currpath));

  /*
   * Allocate a buffer into which we can build stab strings for cases
   * where the stab is continued over multiple lines.
   */
  stabbufflen = 65536;
  stabbuff = (char *) DBG_alloc(stabbufflen);

  strtabinc = 0;
  stabbuff[0] = '\0';
  for(i=0; i < nstab; i++, stab_ptr++ )
    {
      ptr = strs + (unsigned int) stab_ptr->n_un.n_name;
      if( ptr[strlen(ptr) - 1] == '\\' )
        {
          /*
           * Indicates continuation.  Append this to the buffer, and go onto the
           * next record.  Repeat the process until we find a stab without the
           * '/' character, as this indicates we have the whole thing.
           */
          len = strlen(ptr);
          if( strlen(stabbuff) + len > stabbufflen )
            {
              stabbufflen += 65536;
              stabbuff = (char *) DBG_realloc(stabbuff, stabbufflen);
            }
          strncat(stabbuff, ptr, len - 1);
          continue;
        }
      else if( stabbuff[0] != '\0' )
        {
          strcat( stabbuff, ptr);
          ptr = stabbuff;
        }

      if( strchr(ptr, '=') != NULL )
        {
          /*
           * The stabs aren't in writable memory, so copy it over so we are
           * sure we can scribble on it.
           */
          if( ptr != stabbuff )
            {
              strcpy(stabbuff, ptr);
              ptr = stabbuff;
            }
          stab_strcpy(symname, sizeof(symname), ptr);
          if (!DEBUG_ParseTypedefStab(ptr, symname)) {
	    /* skip this definition */
	    stabbuff[0] = '\0';
	    continue;
	  }
        }

      switch(stab_ptr->n_type)
        {
        case N_GSYM:
          /*
           * These are useless with ELF.  They have no value, and you have to
           * read the normal symbol table to get the address.  Thus we
           * ignore them, and when we process the normal symbol table
           * we should do the right thing.
           *
           * With a.out or mingw, they actually do make some amount of sense.
           */
          new_value.addr.seg = 0;
          new_value.type = DEBUG_ParseStabType(ptr);
          new_value.addr.off = load_offset + stab_ptr->n_value;
	  new_value.cookie = DV_TARGET;

          stab_strcpy(symname, sizeof(symname), ptr);
#ifdef __ELF__
          curr_sym = DEBUG_AddSymbol( symname, &new_value, currpath,
                                      SYM_WINE | SYM_DATA | SYM_INVALID );
#else
          curr_sym = DEBUG_AddSymbol( symname, &new_value, currpath,
                                      SYM_WINE | SYM_DATA );
#endif
          break;
        case N_RBRAC:
        case N_LBRAC:
          /*
           * We need to keep track of these so we get symbol scoping
           * right for local variables.  For now, we just ignore them.
           * The hooks are already there for dealing with this however,
           * so all we need to do is to keep count of the nesting level,
           * and find the RBRAC for each matching LBRAC.
           */
          break;
        case N_LCSYM:
        case N_STSYM:
          /*
           * These are static symbols and BSS symbols.
           */
          new_value.addr.seg = 0;
          new_value.type = DEBUG_ParseStabType(ptr);
          new_value.addr.off = load_offset + stab_ptr->n_value;
	  new_value.cookie = DV_TARGET;

          stab_strcpy(symname, sizeof(symname), ptr);
          curr_sym = DEBUG_AddSymbol( symname, &new_value, currpath, 
                                      SYM_WINE | SYM_DATA );
          break;
        case N_PSYM:
          /*
           * These are function parameters.
           */
          if( curr_func != NULL && !in_external_file )
            {
              stab_strcpy(symname, sizeof(symname), ptr);
              curr_loc = DEBUG_AddLocal( curr_func, 0, 
                                         stab_ptr->n_value, 0, 0, symname );
              DEBUG_SetLocalSymbolType( curr_loc, DEBUG_ParseStabType(ptr) );
            }
          break;
        case N_RSYM:
          if( curr_func != NULL && !in_external_file )
            {
              stab_strcpy(symname, sizeof(symname), ptr);
              curr_loc = DEBUG_AddLocal( curr_func, stab_ptr->n_value + 1, 
					 0, 0, 0, symname );
              DEBUG_SetLocalSymbolType( curr_loc, DEBUG_ParseStabType(ptr) );
            }
          break;
        case N_LSYM:
          if( curr_func != NULL && !in_external_file )
            {
              stab_strcpy(symname, sizeof(symname), ptr);
              curr_loc = DEBUG_AddLocal( curr_func, 0, 
					 stab_ptr->n_value, 0, 0, symname );
	      DEBUG_SetLocalSymbolType( curr_loc, DEBUG_ParseStabType(ptr) );
            }
          break;
        case N_SLINE:
          /*
           * This is a line number.  These are always relative to the start
           * of the function (N_FUN), and this makes the lookup easier.
           */
          if( curr_func != NULL && !in_external_file )
            {
#ifdef __ELF__
              DEBUG_AddLineNumber(curr_func, stab_ptr->n_desc, 
                                  stab_ptr->n_value);
#else
#if 0
              /*
               * This isn't right.  The order of the stabs is different under
               * a.out, and as a result we would end up attaching the line
               * number to the wrong function.
               */
              DEBUG_AddLineNumber(curr_func, stab_ptr->n_desc, 
                                  stab_ptr->n_value - curr_func->addr.off);
#endif
#endif
            }
          break;
        case N_FUN:
          /*
           * First, clean up the previous function we were working on.
           */
          DEBUG_Normalize(curr_func);

          /*
           * For now, just declare the various functions.  Later
           * on, we will add the line number information and the
           * local symbols.
           */
          if( !in_external_file)
            {
              stab_strcpy(symname, sizeof(symname), ptr);
	      if (*symname)
		{
		  new_value.addr.seg = 0;
		  new_value.type = DEBUG_ParseStabType(ptr);
		  new_value.addr.off = load_offset + stab_ptr->n_value;
		  new_value.cookie = DV_TARGET;
		  /*
		   * Copy the string to a temp buffer so we
		   * can kill everything after the ':'.  We do
		   * it this way because otherwise we end up dirtying
		   * all of the pages related to the stabs, and that
		   * sucks up swap space like crazy.
		   */
#ifdef __ELF__
		  curr_func = DEBUG_AddSymbol( symname, &new_value, currpath,
					       SYM_WINE | SYM_FUNC | SYM_INVALID );
#else
		  curr_func = DEBUG_AddSymbol( symname, &new_value, currpath,
					       SYM_WINE | SYM_FUNC );
#endif
		} 
	      else
		{
		  /* some GCC seem to use a N_FUN "" to mark the end of a function */
		  curr_func = NULL;
		}
            }
          else
            {
              /*
               * Don't add line number information for this function
               * any more.
               */
              curr_func = NULL;
            }
          break;
        case N_SO:
          /*
           * This indicates a new source file.  Append the records
           * together, to build the correct path name.
           */
#ifndef __ELF__
          /*
           * With a.out, there is no NULL string N_SO entry at the end of
           * the file.  Thus when we find non-consecutive entries,
           * we consider that a new file is started.
           */
          if( last_nso < i-1 )
            {
              currpath[0] = '\0';
              DEBUG_Normalize(curr_func);
              curr_func = NULL;
            }
#endif

          if( *ptr == '\0' )
            {
              /*
               * Nuke old path.
               */
              currpath[0] = '\0';
              DEBUG_Normalize(curr_func);
              curr_func = NULL;
            }
          else
            {
              if (*ptr != '/')
                strcat(currpath, ptr);
              else
                strcpy(currpath, ptr);
              subpath = ptr;
	      DEBUG_ResetIncludes();
            }
          last_nso = i;
          break;
        case N_SOL:
          /*
           * This indicates we are including stuff from an include file.
           * If this is the main source, enable the debug stuff, otherwise
           * ignore it.
           */
          in_external_file = !(subpath == NULL || strcmp(ptr, subpath) == 0);
          break;
        case N_UNDF:
          strs += strtabinc;
          strtabinc = stab_ptr->n_value;
          DEBUG_Normalize(curr_func);
          curr_func = NULL;
          break;
        case N_OPT:
          /*
           * Ignore this.  We don't care what it points to.
           */
          break;
        case N_BINCL:
	   DEBUG_AddInclude(DEBUG_CreateInclude(ptr, stab_ptr->n_value));
	   break;
        case N_EINCL:
	   break;
	case N_EXCL:
	   DEBUG_AddInclude(DEBUG_FindInclude(ptr, stab_ptr->n_value));
	   break;
        case N_MAIN:
          /*
           * Always ignore these.  GCC doesn't even generate them.
           */
          break;
        default:
	  DEBUG_Printf(DBG_CHN_MESG, "Unknown stab type 0x%02x\n", stab_ptr->n_type);
          break;
        }

      stabbuff[0] = '\0';

#if 0
      DEBUG_Printf(DBG_CHN_MESG, "%d %x %s\n", stab_ptr->n_type, 
		   (unsigned int) stab_ptr->n_value,
		   strs + (unsigned int) stab_ptr->n_un.n_name);
#endif
    }

  DEBUG_FreeRegisteredTypedefs();
  DEBUG_FreeIncludes();
  DBG_free(curr_types);
  curr_types = NULL;
  allocated_types = 0;

  return DIL_LOADED;
}

#ifdef __ELF__

/*
 * Walk through the entire symbol table and add any symbols we find there.
 * This can be used in cases where we have stripped ELF shared libraries,
 * or it can be used in cases where we have data symbols for which the address
 * isn't encoded in the stabs.
 *
 * This is all really quite easy, since we don't have to worry about line
 * numbers or local data variables.
 */
static int DEBUG_ProcessElfSymtab(DBG_MODULE* module, char* addr, 
				  u_long load_addr, Elf32_Shdr* symtab, 
				  Elf32_Shdr* strtab)
{
  char		* curfile = NULL;
  struct name_hash * curr_sym = NULL;
  int		  flags;
  int		  i;
  DBG_VALUE       new_value;
  int		  nsym;
  char		* strp;
  char		* symname;
  Elf32_Sym	* symp;

  symp = (Elf32_Sym *) (addr + symtab->sh_offset);
  nsym = symtab->sh_size / sizeof(*symp);
  strp = (char *) (addr + strtab->sh_offset);

  for(i=0; i < nsym; i++, symp++)
    {
      /*
       * Ignore certain types of entries which really aren't of that much
       * interest.
       */
      if( ELF32_ST_TYPE(symp->st_info) == STT_SECTION ||
	  symp->st_shndx == STN_UNDEF )
	{
	  continue;
	}

      symname = strp + symp->st_name;

      /*
       * Save the name of the current file, so we have a way of tracking
       * static functions/data.
       */
      if( ELF32_ST_TYPE(symp->st_info) == STT_FILE )
	{
	  curfile = symname;
	  continue;
	}

      /*
       * See if we already have something for this symbol.
       * If so, ignore this entry, because it would have come from the
       * stabs or from a previous symbol.  If the value is different,
       * we will have to keep the darned thing, because there can be
       * multiple local symbols by the same name.
       */
      if(    (DEBUG_GetSymbolValue(symname, -1, &new_value, FALSE ) == TRUE)
	  && (new_value.addr.off == (load_addr + symp->st_value)) )
	  continue;

      new_value.addr.seg = 0;
      new_value.type = NULL;
      new_value.addr.off = load_addr + symp->st_value;
      new_value.cookie = DV_TARGET;
      flags = SYM_WINE | ((ELF32_ST_TYPE(symp->st_info) == STT_FUNC) 
			  ? SYM_FUNC : SYM_DATA);
      if( ELF32_ST_BIND(symp->st_info) == STB_GLOBAL )
	  curr_sym = DEBUG_AddSymbol( symname, &new_value, NULL, flags );
      else
	  curr_sym = DEBUG_AddSymbol( symname, &new_value, curfile, flags );

      /*
       * Record the size of the symbol.  This can come in handy in
       * some cases.  Not really used yet, however.
       */
      if( symp->st_size != 0 )
	  DEBUG_SetSymbolSize(curr_sym, symp->st_size);
    }

  return TRUE;
}

/*
 * Loads the symbolic information from ELF module stored in 'filename'
 * the module has been loaded at 'load_offset' address, so symbols' address
 * relocation is performed
 * returns 
 *	-1 if the file cannot be found/opened
 *	0 if the file doesn't contain symbolic info (or this info cannot be 
 *	read or parsed)
 *	1 on success
 */
enum DbgInfoLoad DEBUG_LoadElfStabs(DBG_MODULE* module)
{
    enum DbgInfoLoad dil = DIL_ERROR;
    char*	addr = (char*)0xffffffff;
    int		fd = -1;
    struct stat	statbuf;
    Elf32_Ehdr* ehptr;
    Elf32_Shdr* spnt;
    char*	shstrtab;
    int	       	i;
    int		stabsect;
    int		stabstrsect;

    if (module->type != DMT_ELF || ! module->elf_info) {
	DEBUG_Printf(DBG_CHN_ERR, "Bad elf module '%s'\n", module->module_name);
	return DIL_ERROR;
    }

    /* check that the file exists, and that the module hasn't been loaded yet */
    if (stat(module->module_name, &statbuf) == -1) goto leave;
    
    /*
     * Now open the file, so that we can mmap() it.
     */
    if ((fd = open(module->module_name, O_RDONLY)) == -1) goto leave;
    
    dil = DIL_NOINFO;
    /*
     * Now mmap() the file.
     */
    addr = mmap(0, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == (char*)0xffffffff) goto leave;
    
    /*
     * Next, we need to find a few of the internal ELF headers within
     * this thing.  We need the main executable header, and the section
     * table.
     */
    ehptr = (Elf32_Ehdr*) addr;
    spnt = (Elf32_Shdr*) (addr + ehptr->e_shoff);
    shstrtab = (addr + spnt[ehptr->e_shstrndx].sh_offset);
    
    stabsect = stabstrsect = -1;
    
    for (i = 0; i < ehptr->e_shnum; i++) {
	if (strcmp(shstrtab + spnt[i].sh_name, ".stab") == 0)
	    stabsect = i;
	
	if (strcmp(shstrtab + spnt[i].sh_name, ".stabstr") == 0)
	    stabstrsect = i;
    }
    
    if (stabsect == -1 || stabstrsect == -1) {
	DEBUG_Printf(DBG_CHN_WARN, "no .stab section\n");
	goto leave;
    }
    
    /*
     * OK, now just parse all of the stabs.
     */
    if (DEBUG_ParseStabs(addr, 
			 module->elf_info->elf_addr, 
			 spnt[stabsect].sh_offset,
			 spnt[stabsect].sh_size,
			 spnt[stabstrsect].sh_offset,
			 spnt[stabstrsect].sh_size)) {
	dil = DIL_LOADED;
    } else {
	dil = DIL_ERROR;
	DEBUG_Printf(DBG_CHN_WARN, "bad stabs\n");
	goto leave;
    }
    
    for (i = 0; i < ehptr->e_shnum; i++) {
	if (   (strcmp(shstrtab + spnt[i].sh_name, ".symtab") == 0)
	    && (spnt[i].sh_type == SHT_SYMTAB))
	    DEBUG_ProcessElfSymtab(module, addr, module->elf_info->elf_addr,
				   spnt + i, spnt + spnt[i].sh_link);
	
	if (   (strcmp(shstrtab + spnt[i].sh_name, ".dynsym") == 0)
	    && (spnt[i].sh_type == SHT_DYNSYM))
	    DEBUG_ProcessElfSymtab(module, addr, module->elf_info->elf_addr, 
				   spnt + i, spnt + spnt[i].sh_link);
    }

 leave:
    if (addr != (char*)0xffffffff) munmap(addr, statbuf.st_size);
    if (fd != -1) close(fd);
    
    return dil;
}

/*
 * Loads the information for ELF module stored in 'filename'
 * the module has been loaded at 'load_offset' address
 * returns 
 *	-1 if the file cannot be found/opened
 *	0 if the file doesn't contain symbolic info (or this info cannot be 
 *	read or parsed)
 *	1 on success
 */
static enum DbgInfoLoad DEBUG_ProcessElfFile(const char* filename, 
					     unsigned int load_offset,
					     unsigned int* dyn_addr)
{
    enum DbgInfoLoad dil = DIL_ERROR;
    char*	addr = (char*)0xffffffff;
    int		fd = -1;
    struct stat	statbuf;
    Elf32_Ehdr* ehptr;
    Elf32_Shdr* spnt;
    Elf32_Phdr*	ppnt;
    char      * shstrtab;
    int	       	i;
    DBG_MODULE* module = NULL;
    DWORD	size;
    DWORD	delta;

    DEBUG_Printf(DBG_CHN_TRACE, "Processing elf file '%s'\n", filename);

    /* check that the file exists, and that the module hasn't been loaded yet */
    if (stat(filename, &statbuf) == -1) goto leave;
    
    /*
     * Now open the file, so that we can mmap() it.
     */
    if ((fd = open(filename, O_RDONLY)) == -1) goto leave;
    
    /*
     * Now mmap() the file.
     */
    addr = mmap(0, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == (char*)0xffffffff) goto leave;

    dil = DIL_NOINFO;
    
    /*
     * Next, we need to find a few of the internal ELF headers within
     * this thing.  We need the main executable header, and the section
     * table.
     */
    ehptr = (Elf32_Ehdr*) addr;
    spnt = (Elf32_Shdr*) (addr + ehptr->e_shoff);
    shstrtab = (addr + spnt[ehptr->e_shstrndx].sh_offset);

    /* if non relocatable ELF, then remove fixed address from computation
     * otherwise, all addresses are zero based
     */
    delta = (load_offset == 0) ? ehptr->e_entry : 0;

    /* grab size of module once loaded in memory */
    ppnt = (Elf32_Phdr*) (addr + ehptr->e_phoff);
    size = 0;
    for (i = 0; i < ehptr->e_phnum; i++) {
	if (ppnt[i].p_type != PT_LOAD) continue;
	if (size < ppnt[i].p_vaddr - delta + ppnt[i].p_memsz)
	    size = ppnt[i].p_vaddr - delta + ppnt[i].p_memsz;
    }
    
    for (i = 0; i < ehptr->e_shnum; i++) {
	if (strcmp(shstrtab + spnt[i].sh_name, ".bss") == 0 &&
	    spnt[i].sh_type == SHT_NOBITS) {
	    if (size < spnt[i].sh_addr - delta + spnt[i].sh_size)
		size = spnt[i].sh_addr - delta + spnt[i].sh_size;
	}
	if (strcmp(shstrtab + spnt[i].sh_name, ".dynamic") == 0 &&
	    spnt[i].sh_type == SHT_DYNAMIC) {
	    if (dyn_addr) *dyn_addr = spnt[i].sh_addr;
	}
    }
    
    module = DEBUG_RegisterELFModule((load_offset == 0) ? ehptr->e_entry : load_offset, 
				     size, filename);
    if (!module) {
	dil = DIL_ERROR;
	goto leave;
    }

    if ((module->elf_info = DBG_alloc(sizeof(ELF_DBG_INFO))) == NULL) {
	DEBUG_Printf(DBG_CHN_ERR, "OOM\n");
	exit(0);
    }

    module->elf_info->elf_addr = load_offset;
    dil = DEBUG_LoadElfStabs(module);

 leave:
    if (addr != (char*)0xffffffff) munmap(addr, statbuf.st_size);
    if (fd != -1) close(fd);
    if (module) module->dil = dil;
    
    return dil;
}

static enum DbgInfoLoad DEBUG_ProcessElfFileFromPath(const char * filename, 
						     unsigned int load_offset, 
						     unsigned int* dyn_addr, 
						     const char* path)
{
    enum DbgInfoLoad	dil = DIL_ERROR;
    char 	*s, *t, *fn;
    char*	paths = NULL;

    if (!path) return -1;

    for (s = paths = DBG_strdup(path); s && *s; s = (t) ? (t+1) : NULL) {
	t = strchr(s, ':');
	if (t) *t = '\0';
	fn = (char*)DBG_alloc(strlen(filename) + 1 + strlen(s) + 1);
	if (!fn) break;
	strcpy(fn, s );
	strcat(fn, "/");
	strcat(fn, filename);
	dil = DEBUG_ProcessElfFile(fn, load_offset, dyn_addr);
	DBG_free(fn);
	if (dil != DIL_ERROR) break;
	s = (t) ? (t+1) : NULL;
    }

    DBG_free(paths);
    return dil;
}

static enum DbgInfoLoad DEBUG_ProcessElfObject(const char* filename, 
					       unsigned int load_offset,
					       unsigned int* dyn_addr)
{
   enum DbgInfoLoad	dil = DIL_ERROR;

   if (filename == NULL) return DIL_ERROR;
   if (DEBUG_FindModuleByName(filename, DMT_ELF)) return DIL_LOADED;

   dil = DEBUG_ProcessElfFile(filename, load_offset, dyn_addr);

   /* if relative pathname, try some absolute base dirs */
   if (dil == DIL_ERROR && !strchr(filename, '/')) {
      dil = DEBUG_ProcessElfFileFromPath(filename, load_offset, dyn_addr, getenv("PATH"));
      if (dil == DIL_ERROR)
	dil = DEBUG_ProcessElfFileFromPath(filename, load_offset, dyn_addr, getenv("LD_LIBRARY_PATH"));
   }

   DEBUG_ReportDIL(dil, "ELF", filename, load_offset);

   return dil;
}
   
static	BOOL	DEBUG_WalkList(struct r_debug* dbg_hdr)
{
    u_long		lm_addr;
    struct link_map     lm;
    Elf32_Ehdr	        ehdr;
    char		bufstr[256];
    
    /*
     * Now walk the linked list.  In all known ELF implementations,
     * the dynamic loader maintains this linked list for us.  In some
     * cases the first entry doesn't appear with a name, in other cases it
     * does.
     */
    for (lm_addr = (u_long)dbg_hdr->r_map; lm_addr; lm_addr = (u_long)lm.l_next) {
	if (!DEBUG_READ_MEM_VERBOSE((void*)lm_addr, &lm, sizeof(lm)))
	    return FALSE;
	if (lm.l_addr != 0 &&
	    DEBUG_READ_MEM_VERBOSE((void*)lm.l_addr, &ehdr, sizeof(ehdr)) &&
	    ehdr.e_type == ET_DYN && /* only look at dynamic modules */
	    lm.l_name != NULL &&
	    DEBUG_READ_MEM_VERBOSE((void*)lm.l_name, bufstr, sizeof(bufstr))) {
	    bufstr[sizeof(bufstr) - 1] = '\0';
	    DEBUG_ProcessElfObject(bufstr, (unsigned)lm.l_addr, NULL);
	}
    }
    
    return TRUE;
}

static BOOL DEBUG_RescanElf(void)
{
    struct r_debug        dbg_hdr;

    if (!DEBUG_CurrProcess || 
	!DEBUG_READ_MEM_VERBOSE((void*)DEBUG_CurrProcess->dbg_hdr_addr, &dbg_hdr, sizeof(dbg_hdr)))
       return FALSE;

    switch (dbg_hdr.r_state) {
    case RT_CONSISTENT:	
       DEBUG_WalkList(&dbg_hdr);
       break;
    case RT_ADD:
       break;
    case RT_DELETE:
       /* FIXME: this is not currently handled, would need some kind of mark&sweep algo */
      break;
    }
    return FALSE;
}

enum DbgInfoLoad	DEBUG_ReadExecutableDbgInfo(const char* exe_name)
{
    Elf32_Dyn		dyn;
    struct r_debug      dbg_hdr;
    enum DbgInfoLoad	dil = DIL_NOINFO;
    unsigned int	dyn_addr;
    
    /*
     * Make sure we can stat and open this file.
     */
    if (exe_name == NULL) goto leave;
    DEBUG_ProcessElfObject(exe_name, 0, &dyn_addr);
    
    do {
	if (!DEBUG_READ_MEM_VERBOSE((void*)dyn_addr, &dyn, sizeof(dyn)))
	    goto leave;
	dyn_addr += sizeof(dyn);
    } while (dyn.d_tag != DT_DEBUG && dyn.d_tag != DT_NULL);
    if (dyn.d_tag == DT_NULL) goto leave;
    
    /*
     * OK, now dig into the actual tables themselves.
     */
    if (!DEBUG_READ_MEM_VERBOSE((void*)dyn.d_un.d_ptr, &dbg_hdr, sizeof(dbg_hdr)))
	goto leave;
    
    assert(!DEBUG_CurrProcess->dbg_hdr_addr);
    DEBUG_CurrProcess->dbg_hdr_addr = (u_long)dyn.d_un.d_ptr;
    
    if (dbg_hdr.r_brk) {
	DBG_VALUE	value;
	
	DEBUG_Printf(DBG_CHN_TRACE, "Setting up a breakpoint on r_brk(%lx)\n",
		     (unsigned long)dbg_hdr.r_brk);
	
	DEBUG_SetBreakpoints(FALSE);
	value.type = NULL;
	value.cookie = DV_TARGET;
	value.addr.seg = 0;
	value.addr.off = (DWORD)dbg_hdr.r_brk;
	DEBUG_AddBreakpoint(&value, DEBUG_RescanElf);
	DEBUG_SetBreakpoints(TRUE);
    }
    
    dil = DEBUG_WalkList(&dbg_hdr);
    
 leave:
    return dil;
}

#else	/* !__ELF__ */

int	DEBUG_ReadExecutableDbgInfo(const char* exe_name)
{
  return FALSE;
}

#endif  /* __ELF__ */
