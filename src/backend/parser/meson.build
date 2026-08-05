# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'analyze.c',
  'parse_agg.c',
  'parse_clause.c',
  'parse_coerce.c',
  'parse_collate.c',
  'parse_cte.c',
  'parse_enr.c',
  'parse_expr.c',
  'parse_func.c',
  'parse_jsontable.c',
  'parse_merge.c',
  'parse_node.c',
  'parse_oper.c',
  'parse_param.c',
  'parse_relation.c',
  'parse_target.c',
  'parse_type.c',
  'parse_utilcmd.c',
  'scansup.c',
)

# Build a small utility static lib for the parser. The generation of the
# parser is slow, and building this separately avoids other parts of the
# backend having to wait till gram.h is generated.
parser_sources = files('parser.c', 'parsereng.c')

backend_scanner = custom_target('scan',
  input: 'scan.l',
  output: 'scan.c',
  command: [flex_cmd, '--no-backup', '--', '-CF', '-p', '-p'],
)
generated_sources += backend_scanner
parser_sources += backend_scanner

backend_parser = custom_target('gram',
  input: 'gram.y',
  kwargs: bison_kw,
)
generated_sources += backend_parser.to_list()
parser_sources += backend_parser

# MySQL scanner generation (consumed by the loadable mysql_parser module)
mys_backend_scanner = custom_target('mys_scan',
  input: 'mysql/mys_scan.l',
  output: 'mys_scan.c',
  command: [flex_cmd, '--no-backup', '--', '-CF', '-p', '-p'],
)

# MySQL grammar generation
mys_backend_parser = custom_target('mys_gram',
  input: 'mysql/mys_gram.y',
  kwargs: bison_kw,
)

# MySQL keyword hash generation (mys_kwlist_d.h)
mys_kwlist_d = custom_target('mys_kwlist_d',
  input: join_paths(meson.project_source_root(), 'src', 'include', 'parser', 'mysql', 'mys_kwlist.h'),
  output: 'mys_kwlist_d.h',
  command: [perl, join_paths(meson.project_source_root(), 'src', 'tools', 'gen_keywordlist.pl'),
            '--extern', '--output', '@OUTDIR@', '--varname=MysScanKeywords', '@INPUT@'],
)

# MySQL compatibility parser as a loadable shared module.  The kernel
# keeps the ParserRoutine vtable skeleton; the module registers itself
# via _PG_init (see mysql/mys_parser.c).  Mirrors the mysm module pattern.
mysql_parser = shared_module('mysql_parser',
  [
    'mysql/mys_expr_transform.c',
    'mysql/mys_gram_globals.c',
    'mysql/mys_keywords.c',
    'mysql/mys_namespace.c',
    'mysql/mys_parse_utilcmd.c',
    'mysql/mys_parser.c',
    mys_backend_scanner,
    mys_backend_parser,
    mys_kwlist_d,
  ],
  include_directories: postgres_inc,
  c_args: ['-fvisibility=default'],
  install: true,
  install_dir: dir_lib_pkg,
  name_prefix: '',
)
parser = static_library('parser',
  parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += parser
