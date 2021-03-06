#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <errno.h>

#include "str_tree.h"
#include "pexports.h"

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#ifdef _WIN32
#define PATH_SEPARATOR ';'
#else
#define PATH_SEPARATOR ':'
#endif

/* get pointer to section header n */
#define IMAGE_SECTION_HDR(n) \
  ((IMAGE_SECTION_HEADER *)((char *) nt_hdr32 \
    + 4 + sizeof(IMAGE_FILE_HEADER) + nt_hdr32->FileHeader.SizeOfOptionalHeader \
    + n * sizeof(IMAGE_SECTION_HEADER)) \
  )

/* convert relative virtual address to a useable pointer */
#define RVA_TO_PTR(rva,type) ((type)(rva_to_ptr((uint32_t)(rva))))

typedef struct str_list {
  char *s;
  struct str_list *next;
} str_list;

extern void yyparse(void);

static void *xmalloc(size_t count);
static void add_path_list(char *path);
static const char *find_file(const char *name);
static void str_list_add(str_list **head, const char *s);
static void parse_headers();
static void dump_symbol(char *name, int ord, uint32_t rva);

static const char mz_sign[2] = {'M','Z'};
static const char pe_sign[4] = {'P','E','\0','\0'};
static const char exp_sign[6] = {'.','e','d','a','t','a'};

static IMAGE_DOS_HEADER   *dos_hdr;
static IMAGE_NT_HEADERS32 *nt_hdr32;
static IMAGE_NT_HEADERS64 *nt_hdr64;

static char *filename = NULL;
static char *program_name;
static char *cpp = "gcc -E -xc-header";

str_tree *symbols = NULL;
static str_list *inc_path = NULL;
static str_list *header_files = NULL;

static int verbose = 0;
static int ordinal_flag = 0;

extern FILE *yyin;

int
main(int argc, char *argv[])
{
  IMAGE_SECTION_HEADER *section;
  uint32_t exp_rva, exp_size;
  int i;

# if defined(_WIN32) && !defined(_WIN64)
  /*
   * If running on 64-bit Windows and built as a 32-bit process, try
   * disable Wow64 file system redirection, so that we can open DLLs
   * in the real system32 folder if requested.
   */
  void *old_redirection;
  void *kernel32;

  extern __declspec(dllimport) void __stdcall *GetModuleHandleA(char *name);
  extern __declspec(dllimport) void __stdcall *GetProcAddress(void *module, char *name);

  int32_t (__stdcall *pWow64DisableWow64FsRedirection) (void **old_value);

  kernel32 = GetModuleHandleA("kernel32.dll");
  pWow64DisableWow64FsRedirection = GetProcAddress(kernel32, "Wow64DisableWow64FsRedirection");

  if (pWow64DisableWow64FsRedirection)
    pWow64DisableWow64FsRedirection(&old_redirection);
# endif

  program_name = argv[0];

  /* add standard include paths */
  add_path_list(getenv("C_INCLUDE_PATH"));
  add_path_list(getenv("CPLUS_INCLUDE_PATH"));
#if 0
  /* since when does PATH have anything to do with headers? */
  add_path_list(getenv("PATH"));
#endif

  /* parse command line */
  for ( i = 1; i < argc; i++ )
    {
      if (argv[i][0] == '-')
        {
          switch (argv[i][1])
            {
            case 'v':
              verbose = 1;
              break;
            case 'o':
              ordinal_flag = 1;
              break;
            case 'h':
              if (!argv[++i])
                {
                  fprintf(stderr, "-h requires an argument\n");
                  return 1;
                }
              str_list_add(&header_files, argv[i]);
              break;
            case 'p':
              if (!argv[++i])
                {
                  fprintf(stderr, "-p requires an argument\n");
                  return 1;
                }
              cpp = argv[i];
              break;
            default:
              fprintf(stderr, "%s: Unknown option: %s\n",
                      program_name, argv[i]);
              return 1;
            }
        }
      else
        filename = argv[i];
    }

  if (filename == NULL)
    {
      printf(
	"PExports %s; Originally written 1998, Anders Norlander\n"
   	"Updated 1999, Paul Sokolovsky, 2008, Tor Lillqvist, 2013, 2015, Keith Marshall\n"
	"Copyright (C) 1998, 1999, 2008, 2013, 2015, MinGW.org Project\n\n"
	"This program is free software; you may redistribute it under the terms of\n"
	"the GNU General Public License.  This program has absolutely no warranty.\n"

	"\nUsage: %s [-v] [-o] [-h header] [-p preprocessor] dll\n"
	"  -h\tparse header\n"
	"  -o\tprint ordinals\n"
	"  -p\tset preprocessor program\n"
	"  -v\tverbose mode\n"
	"\nReport bugs as directed at %s\n",
	PACKAGE_VERSION_STRING, program_name, PACKAGE_BUG_REPORT);
      return 1;
    }

  /* parse headers and build symbol tree */
  parse_headers();

  /* load file */
  if( (dos_hdr = load_pe_image(filename)) == NULL )
    {
      fprintf(stderr, "%s: %s: could not load PE image\n",
              program_name, filename);
      return 1;
    }

  nt_hdr32 = (IMAGE_NT_HEADERS32 *) ((char *) dos_hdr + dos_hdr->e_lfanew);
  nt_hdr64 = (IMAGE_NT_HEADERS64 *) nt_hdr32;

  if (nt_hdr32->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
    exp_rva = nt_hdr32->OptionalHeader.DataDirectory[0].VirtualAddress;
    exp_size = nt_hdr32->OptionalHeader.DataDirectory[0].Size;
  }else{
    exp_rva = nt_hdr64->OptionalHeader.DataDirectory[0].VirtualAddress;
    exp_size = nt_hdr64->OptionalHeader.DataDirectory[0].Size;
  }

  if (verbose)
    {
      for (i = 0; i < nt_hdr32->FileHeader.NumberOfSections; i++)
        {
	  section = IMAGE_SECTION_HDR(i);
          printf("; %-8.8s: RVA: %08x, File offset: %08x\n",
                 section->Name,
                 section->VirtualAddress,
                 section->PointerToRawData);
        }
    }

  /* Look for export section */
  for (i = 0; i < nt_hdr32->FileHeader.NumberOfSections; i++)
    {
      section = IMAGE_SECTION_HDR(i);
      if (memcmp(section->Name, exp_sign, sizeof(exp_sign)) == 0)
        dump_exports(section->VirtualAddress, exp_size);
      else if ((exp_rva >= section->VirtualAddress) &&
          (exp_rva < (section->VirtualAddress + section->SizeOfRawData)))
        dump_exports(exp_rva, exp_size);
    }

  free(dos_hdr);
  return 0;
}

/* dump exported symbols on stdout */
void
dump_exports(uint32_t exports_rva, uint32_t exports_size)
{
  IMAGE_SECTION_HEADER *section;
  IMAGE_EXPORT_DIRECTORY *exports;
  char *export_name;
  uint16_t *ordinal_table;
  uint32_t *name_table;
  uint32_t *function_table;
  int i;
  static int first = 1;

  section = find_section(exports_rva);

  if (verbose)
    printf("; Reading exports from section: %s\n",
            section->Name);

  exports = RVA_TO_PTR(exports_rva, IMAGE_EXPORT_DIRECTORY *);

  /* set up various pointers */
  export_name = RVA_TO_PTR(exports->Name, char *);
  name_table = RVA_TO_PTR(exports->AddressOfNames, uint32_t *);
  ordinal_table = RVA_TO_PTR(exports->AddressOfNameOrdinals, uint16_t *);
  function_table = RVA_TO_PTR(exports->AddressOfFunctions, void *);

  if (verbose)
    {
      printf("; Export table: %s\n", export_name);
      printf("; Ordinal base: %d\n", exports->Base);
      printf("; Ordinal table RVA: %08x\n",
              exports->AddressOfNameOrdinals);
      printf("; Name table RVA: %07x\n",
	     exports->AddressOfNames);
      printf("; Export address table RVA: %08x\n",
             exports->AddressOfFunctions);
    }

  if (first)
    {
      printf("LIBRARY %s\n", export_name);
      printf("EXPORTS\n");
      first = 0;
    }
  else
      printf("; LIBRARY %s\n", export_name);

  for (i = 0; i < exports->NumberOfNames; i++)
    {
      dump_symbol(RVA_TO_PTR(name_table[i],char*),
                  ordinal_table[i] + exports->Base,
                  function_table[ordinal_table[i]]);

      int f_off = ordinal_table[i];

      if(function_table[f_off] >= exports_rva && function_table[f_off] < (exports_rva + exports_size) && verbose) {
        printf(" ; Forwarder (%s)", RVA_TO_PTR(function_table[f_off], char*));
      }

      printf("\n");
    }

  for (i = 0; i < exports->NumberOfFunctions; i++)
    {
      if ( (function_table[i] >= exports_rva) &&
           (function_table[i] < (exports_rva + exports_size)))
        {
          int name_present = 0, n;

          for(n = 0; n < exports->NumberOfNames; n++) {
            if(ordinal_table[n] == i) {
              name_present = 1;
              break;
            }
          }

          if(!name_present) {
            dump_symbol(strchr(RVA_TO_PTR(function_table[i],char*), '.')+1, i + exports->Base, function_table[i]);

            printf(" ; WARNING: Symbol name guessed from forwarder (%s)\n", RVA_TO_PTR(function_table[i], char*));
          }
        }
    }
}

static void
dump_symbol(char *name, int ord, uint32_t rva)
{
  char s[256];
  str_tree *symbol = str_tree_find(symbols, name);
  /* if a symbol was found, emit size of stack */
  if (symbol)
    sprintf(s, "%s@%"PRIdPTR, name, (intptr_t)(symbol->extra));
  else
    sprintf(s, "%s", name);

  /* output ordinal */
  if (ordinal_flag)
    printf("%-24s\t@%d", s, ord);
  else
    printf("%s", s);

  {
    IMAGE_SECTION_HEADER *s = find_section(rva);

    /* Stupid msvc doesn't have .bss section, it spews uninitilized data
     * to no section
     */
    if (!s) { printf(" DATA"); if (verbose) printf (" ; no section"); }
    else
    {
      if (!(s->Characteristics&IMAGE_SCN_CNT_CODE)) printf(" DATA");
      if (verbose) printf (" ; %.8s",s->Name);
    }
  }

  if (verbose)
    printf(" ; RVA %08x", rva);
}

/* get section to which rva points */
IMAGE_SECTION_HEADER *find_section(uint32_t rva)
{
  int i;
  IMAGE_SECTION_HEADER *section = IMAGE_SECTION_HDR(0);
  for (i = 0; i < nt_hdr32->FileHeader.NumberOfSections; i++, section++)
    if ((rva >= section->VirtualAddress) &&
        (rva <= (section->VirtualAddress + section->SizeOfRawData)))
      return section;
  return NULL;
}

/* convert rva to pointer into loaded file */
void *
rva_to_ptr(uint32_t rva)
{
  IMAGE_SECTION_HEADER *section = find_section(rva);
  return (section->PointerToRawData != 0)
    ? (char *)(dos_hdr) + rva - section->VirtualAddress + section->PointerToRawData
    : NULL;
}

/* Load a portable executable into memory */
IMAGE_DOS_HEADER *load_pe_image(const char *filename)
{
#ifdef _MSC_VER
  struct _stat32 st;
#else
  struct stat st;
#endif
  IMAGE_DOS_HEADER *phdr;
  FILE *f;

  if (stat(filename, &st) == -1)
    {
      perror("stat");
      return NULL;
    }

  phdr = (IMAGE_DOS_HEADER *) xmalloc(st.st_size);

  f = fopen(filename, "rb");

  if (f == NULL)
    {
      perror("fopen");
      free(phdr);
      return NULL;
    }

  if (fread(phdr, st.st_size, 1, f) != 1)
    {
      perror("fread");
      free(phdr);
      phdr = NULL;
    }
  else if (memcmp(mz_sign, phdr, sizeof(mz_sign)) != 0)
    {
      fprintf(stderr, "No MZ signature\n");
      free(phdr);
      phdr = NULL;
    }
  else if (memcmp((char *) phdr + phdr->e_lfanew, pe_sign, sizeof(pe_sign)) != 0)
    {
      fprintf(stderr, "No PE signature\n");
      free(phdr);
      phdr = NULL;
    }

  fclose(f);
  return phdr;
}

/* parse headers to build symbol tree */
void
parse_headers()
{
#ifdef _MSC_VER
#define popen _popen
#define pclose _pclose
#endif

  str_list *header;
  char *cpp_cmd;
  int len;
  FILE *f;

  header = header_files;
  if (!header)
    return;

  /* construct command line */
  cpp_cmd = strdup(cpp);
  if (cpp_cmd == NULL)
    {
      fprintf(stderr, "%s: out of memory\n", program_name);
      exit(1);
    }
  len = strlen(cpp_cmd);

  while (header)
    {
      const char *fullname = find_file(header->s);
      int tmp;
      if (fullname == NULL)
        {
          perror(header->s);
          exit(1);
        }
      tmp = strlen(fullname) + 10;
      cpp_cmd = realloc(cpp_cmd, len + tmp);
      if (cpp_cmd == NULL)
        {
          fprintf(stderr, "%s: out of memory\n", program_name);
          exit(1);
        }
      if (!header->next)
        sprintf(cpp_cmd + len, " %s", fullname);
      else
        sprintf(cpp_cmd + len, " -include %s", fullname);
      len += tmp;
      header = header->next;
    }
  cpp_cmd[len] = '\0';

  if (verbose)
    printf("; %s\n", cpp_cmd);

  /* Run preprocessor.
     Note: CRTDLL messes up stdout when popen is called so
     if you try to pipe output through another program
     with | it will hang. Redirect it to a file instead
     and pass that file to the program (more,less or whatever).
     This does not apply to cygwin.
  */
  f = popen(cpp_cmd, "r");
  free(cpp_cmd);
  if (f == NULL)
    {
      fprintf(stderr, "%s: %s: could not execute\n", program_name, cpp_cmd);
      exit(1);
    }
  yyin = f;
  yyparse();
  pclose(f);
}

/* allocate memory; abort on failure */
static void *xmalloc(size_t count)
{
  void *p = malloc(count);
  if (p == NULL)
    {
      fprintf(stderr, "%s: out of memory\n", program_name);
      exit(1);
    }
  return p;
}

/* add string to end of list */
static void
str_list_add(str_list **head, const char *s)
{
  str_list *node = xmalloc(sizeof(str_list));
  node->s = strdup(s);
  node->next = NULL;
  if (!*head)
    {
      *head = node;
    }
  else
    {
      str_list *p = *head;
      while (p->next)
        p = p->next;
      p->next = node;
    }
}

/* find a file in include path */
static const char *
find_file(const char *name)
{
  static char fullname[PATH_MAX];
  FILE *f = fopen(name, "r");
  str_list *path = inc_path;
  if (f != NULL)
    {
      fclose(f);
      return name;
    }
  while (path)
    {
      strcpy(fullname, path->s);
      strcat(fullname, "/");
      strcat(fullname, name);
      f = fopen(fullname, "r");
      if (f != NULL)
        {
          fclose(f);
          return fullname;
        }
      path = path->next;
    }
  errno = ENOENT;
  return NULL;
}

/* add a environment-style path list to list of include paths */
static void
add_path_list(char *path)
{
  char *p = path;
  if (!p)
    return;
  while (*p)
    {
      if (*p == PATH_SEPARATOR)
        {
          *p = '\0';
          str_list_add(&inc_path, path);
          path = p + 1;
          *p = PATH_SEPARATOR;
        }
      p++;
    }
  if (p[-1] != PATH_SEPARATOR)
    str_list_add(&inc_path, path);
}
