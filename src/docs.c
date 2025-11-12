#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "docs.h"
#include "message.h"

static SV docs_package_path(SV path) {
    if (sv_match(path, ".")) {
        return sv_from_cstr("index");
    }

    return path;
}

typedef enum {
    STYLE_NORMAL,
    STYLE_STRING,
    STYLE_COMMENT,
    STYLE_KEYWORD,
    STYLE_PACKAGE,
    STYLE_CONSTANT,
    STYLE_FN,
    STYLE_VAR,
    STYLE_TYPE,
} Style;

typedef struct {
    SV dir;
    SV std;

    FILE     *f;
    SB        sb;
    Package  *current;
    Comments *comments;

    DynamicArray(SV) packages;
} Docs;

static void docs_format_type(Docs *d, Type type);

static void docs_format_style_begin(Docs *d, Style style) {
    switch (style) {
    case STYLE_NORMAL:
        break;

    case STYLE_STRING:
        fprintf(d->f, "<span class='string'>");
        break;

    case STYLE_COMMENT:
        fprintf(d->f, "<span class='comment'>");
        break;

    case STYLE_KEYWORD:
        fprintf(d->f, "<span class='keyword'>");
        break;

    case STYLE_PACKAGE:
        fprintf(d->f, "<span class='package'>");
        break;

    case STYLE_CONSTANT:
        fprintf(d->f, "<span class='constant'>");
        break;

    case STYLE_FN:
        fprintf(d->f, "<span class='function'>");
        break;

    case STYLE_VAR:
        fprintf(d->f, "<span class='variable'>");
        break;

    case STYLE_TYPE:
        fprintf(d->f, "<span class='type'>");
        break;
    }
}

static void docs_format_style_end(Docs *d, Style style) {
    if (style != STYLE_NORMAL) {
        fprintf(d->f, "</span>");
    }
}

static PrintfLike(3) void docs_printf_safe(Docs *d, Style style, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = temp_alloc(n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    docs_format_style_begin(d, style);
    for (size_t i = 0; i < (size_t) n; i++) {
        const char it = result[i];
        switch (it) {
        case '&':
            fprintf(d->f, "&amp;");
            break;

        case '<':
            fprintf(d->f, "&lt;");
            break;

        case '>':
            fprintf(d->f, "&gt;");
            break;

        case '\'':
            fprintf(d->f, "&apos;");
            break;

        case '"':
            fprintf(d->f, "&quot;");
            break;

        default:
            fputc(it, d->f);
            break;
        }
    }
    docs_format_style_end(d, style);
}

static void docs_format_identifier(Docs *d, Style style, SV sv, Package *package) {
    const char *delim = "#";
    if (style == STYLE_TYPE) {
        delim = "#@";
    }

    if (package) {
        if (package != d->current) {
            const SV path = docs_package_path(package->relative_path);
            docs_format_style_begin(d, STYLE_PACKAGE);
            fprintf(d->f, "<a href='" SVFmt "'>" SVFmt "</a>", SVArg(path), SVArg(package->name.sv));
            docs_format_style_end(d, style);

            fprintf(d->f, "::");

            docs_format_style_begin(d, style);
            fprintf(d->f, "<a href='" SVFmt "%s" SVFmt "'>" SVFmt "</a>", SVArg(path), delim, SVArg(sv), SVArg(sv));
            docs_format_style_end(d, style);
        } else {
            docs_format_style_begin(d, style);
            fprintf(d->f, "<a href='%s" SVFmt "'>" SVFmt "</a>", delim, SVArg(sv), SVArg(sv));
            docs_format_style_end(d, style);
        }
    } else {
        docs_printf_safe(d, style, SVFmt, SVArg(sv));
    }
}

static void docs_format_generics(Docs *d, Node *generics) {
    if (generics) {
        docs_printf_safe(d, STYLE_NORMAL, "[");
        for (Node *it = generics; it; it = it->next) {
            docs_format_type(d, it->type);
            if (it->next) {
                docs_printf_safe(d, STYLE_NORMAL, ", ");
            }
        }
        docs_printf_safe(d, STYLE_NORMAL, "]");
    }
}

static void docs_format_fn_signature(Docs *d, NodeFn *fn) {
    docs_format_generics(d, fn->generics.head);
    docs_printf_safe(d, STYLE_NORMAL, "(");
    for (const Node *it = fn->args.head; it; it = it->next) {
        if (fn->is_method && it == fn->args.head) {
            continue;
        }

        if (it->token.sv.count) {
            docs_printf_safe(d, STYLE_NORMAL, SVFmt " ", SVArg(it->token.sv));
        }

        if (!it->next && fn->variadic == VARIADIC_TYPED) {
            docs_printf_safe(d, STYLE_NORMAL, "...");

            assert(it->type.kind == TYPE_SLICE);
            docs_format_type(d, *it->type.spec_type);
        } else {
            docs_format_type(d, it->type);
        }

        if (it->next) {
            docs_printf_safe(d, STYLE_NORMAL, ", ");
        } else if (fn->variadic == VARIADIC_UNTYPED) {
            docs_printf_safe(d, STYLE_NORMAL, ", ...");
        }
    }
    docs_printf_safe(d, STYLE_NORMAL, ")");

    if (fn->ret) {
        docs_printf_safe(d, STYLE_NORMAL, " ");
        docs_format_type(d, fn->ret->type);
    }
}

static void docs_format_type(Docs *d, Type type) {
    for (size_t i = 0; i < type.ref; i++) {
        docs_printf_safe(d, STYLE_NORMAL, "&");
    }

    switch (type.kind) {
    case TYPE_UNIT:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("()"), NULL);
        break;

    case TYPE_BOOL:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("bool"), NULL);
        break;

    case TYPE_CHAR:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("char"), NULL);
        break;

    case TYPE_I8:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("i8"), NULL);
        break;

    case TYPE_I16:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("i16"), NULL);
        break;

    case TYPE_I32:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("i32"), NULL);
        break;

    case TYPE_I64:
    case TYPE_INT:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("i64"), NULL);
        break;

    case TYPE_U8:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("u8"), NULL);
        break;

    case TYPE_U16:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("u16"), NULL);
        break;

    case TYPE_U32:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("u32"), NULL);
        break;

    case TYPE_U64:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("u64"), NULL);
        break;

    case TYPE_F32:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("f32"), NULL);
        break;

    case TYPE_F64:
    case TYPE_FLOAT:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("f64"), NULL);
        break;

    case TYPE_RAWPTR:
        docs_format_identifier(d, STYLE_TYPE, sv_from_cstr("rawptr"), NULL);
        break;

    case TYPE_FN:
        docs_printf_safe(d, STYLE_KEYWORD, "fn");
        docs_format_fn_signature(d, (NodeFn *) type.spec_node);
        break;

    case TYPE_ARRAY:
        docs_printf_safe(d, STYLE_NORMAL, "[");
        docs_printf_safe(d, STYLE_CONSTANT, "%zu", type.spec_count);
        docs_printf_safe(d, STYLE_NORMAL, "]");
        docs_format_type(d, *type.spec_type);
        break;

    case TYPE_SLICE:
        docs_printf_safe(d, STYLE_NORMAL, "[]");
        docs_format_type(d, *type.spec_type);
        break;

    case TYPE_DSLICE:
        docs_printf_safe(d, STYLE_NORMAL, "[..]");
        docs_format_type(d, *type.spec_type);
        break;

    case TYPE_TRAIT: {
        NodeTrait *trait = (NodeTrait *) type.spec_node;
        docs_format_identifier(d, STYLE_TYPE, trait->node.token.sv, trait->package);
    } break;

    case TYPE_STRUCT: {
        NodeStruct *structt = (NodeStruct *) type.spec_node;
        docs_format_identifier(d, STYLE_TYPE, structt->node.token.sv, structt->package);
        if (type.spec_struct_instance) {
            docs_format_generics(d, type.spec_struct_instance->generics);
        }
    } break;

    case TYPE_GENERIC:
        docs_printf_safe(d, STYLE_TYPE, SVFmt, SVArg(type.spec_node->token.sv));
        break;

    default:
        unreachable();
    }
}

static void docs_emit_fn(Docs *d, Node *n) {
    NodeFn *fn = (NodeFn *) n;
    docs_printf_safe(d, STYLE_KEYWORD, "fn ");
    if (fn->is_method) {
        docs_printf_safe(d, STYLE_NORMAL, "(" SVFmt " ", SVArg(fn->args.head->token.sv));
        docs_format_type(d, fn->args.head->type);
        docs_printf_safe(d, STYLE_NORMAL, ") ");
    }

    docs_printf_safe(d, STYLE_FN, SVFmt, SVArg(n->token.sv));
    docs_format_fn_signature(d, fn);
    docs_printf_safe(d, STYLE_NORMAL, "\n");
}

static void docs_emit_var(Docs *d, Node *n) {
    docs_printf_safe(d, STYLE_KEYWORD, "var ");
    docs_printf_safe(d, STYLE_VAR, SVFmt, SVArg(n->token.sv));
    docs_printf_safe(d, STYLE_NORMAL, " ");
    docs_format_type(d, n->type);
    docs_printf_safe(d, STYLE_NORMAL, "\n");
}

static_assert(COUNT_NODES == 28, "");
static void docs_emit_type(Docs *d, Node *n) {
    switch (n->kind) {
    case NODE_TYPE: {
        NodeType *type = (NodeType *) n;
        docs_printf_safe(d, STYLE_KEYWORD, "type ");
        docs_printf_safe(d, STYLE_TYPE, SVFmt, SVArg(n->token.sv));
        docs_format_generics(d, type->generics.head);
        docs_printf_safe(d, STYLE_NORMAL, " ");
        docs_format_type(d, n->type);
        docs_printf_safe(d, STYLE_NORMAL, "\n");
    } break;

    case NODE_STRUCT: {
        NodeStruct *structt = (NodeStruct *) n;
        docs_printf_safe(d, STYLE_KEYWORD, "struct ");
        docs_printf_safe(d, STYLE_TYPE, SVFmt, SVArg(n->token.sv));
        docs_format_generics(d, structt->generics.head);
        docs_printf_safe(d, STYLE_NORMAL, " {\n");
        for (Node *it = structt->fields.head; it; it = it->next) {
            docs_printf_safe(d, STYLE_NORMAL, "\t" SVFmt " ", SVArg(it->token.sv));
            docs_format_type(d, it->type);
            docs_printf_safe(d, STYLE_NORMAL, "\n");
        }
        docs_printf_safe(d, STYLE_NORMAL, "}\n");
    } break;

    case NODE_TRAIT: {
        NodeTrait *trait = (NodeTrait *) n;
        docs_printf_safe(d, STYLE_KEYWORD, "trait ");
        docs_printf_safe(d, STYLE_TYPE, SVFmt, SVArg(n->token.sv));
        docs_printf_safe(d, STYLE_NORMAL, " {\n");
        for (Node *it = trait->fns.head; it; it = it->next) {
            docs_printf_safe(d, STYLE_NORMAL, "\t" SVFmt, SVArg(it->token.sv));
            docs_format_fn_signature(d, (NodeFn *) it);
            docs_printf_safe(d, STYLE_NORMAL, "\n");
        }
        docs_printf_safe(d, STYLE_NORMAL, "}\n");
    } break;

    default:
        unreachable();
    }
}

static void docs_emit_const(Docs *d, Node *n) {
    NodeConst *constt = (NodeConst *) n;
    docs_printf_safe(d, STYLE_KEYWORD, "const ");
    docs_printf_safe(d, STYLE_VAR, SVFmt, SVArg(n->token.sv));
    docs_printf_safe(d, STYLE_NORMAL, " ");
    docs_format_type(d, n->type);
    docs_printf_safe(d, STYLE_NORMAL, " = ");

    if (constt->value.is_string) {
        docs_format_style_begin(d, STYLE_STRING);
        docs_printf_safe(d, STYLE_NORMAL, "\"");
        for (size_t i = 0; i < constt->value.as.sv.count; i++) {
            print_quoted_char(d->f, constt->value.as.sv.data[i], '"');
        }
        docs_printf_safe(d, STYLE_NORMAL, "\"");
        docs_format_style_end(d, STYLE_STRING);
    } else if (n->type.kind == TYPE_CHAR) {
        docs_format_style_begin(d, STYLE_STRING);
        docs_printf_safe(d, STYLE_NORMAL, "'");
        print_quoted_char(d->f, constt->value.as.integer, '\'');
        docs_printf_safe(d, STYLE_NORMAL, "'");
        docs_format_style_end(d, STYLE_STRING);
    } else if (n->type.kind == TYPE_F64 || n->type.kind == TYPE_FLOAT) {
        docs_printf_safe(d, STYLE_CONSTANT, "%.14g", constt->value.as.floating);
    } else if (n->type.kind == TYPE_F32) {
        docs_printf_safe(d, STYLE_CONSTANT, "%g", constt->value.as.floating);
    } else if (n->type.kind == TYPE_BOOL) {
        docs_printf_safe(d, STYLE_CONSTANT, "%s", constt->value.as.integer ? "true" : "false");
    } else if (type_is_signed(n->type)) {
        docs_printf_safe(d, STYLE_CONSTANT, "%ld", constt->value.as.integer);
    } else {
        docs_printf_safe(d, STYLE_CONSTANT, "%zu", constt->value.as.integer);
    }

    docs_printf_safe(d, STYLE_NORMAL, "\n");
}

static bool find_in_list(const char **list, size_t count, SV sv) {
    for (size_t i = 0; i < count; i++) {
        if (sv_match(sv, list[i])) {
            return true;
        }
    }

    return false;
}

static void docs_emit_code(Docs *d, Pos pos) {
    Lexer lexer = {0};
    lexer_init(&lexer, pos, (SV) {.data = d->sb.data, .count = d->sb.count});
    const char *last = d->sb.data;

    fprintf(d->f, "<pre>\n");
    while (true) {
        Token token = lexer_next(&lexer);
        Style style = STYLE_NORMAL;
        switch (token.kind) {
        case TOKEN_EOF:
        case TOKEN_DOT:
        case TOKEN_COLON:
        case TOKEN_COMMA:
        case TOKEN_RANGE:
        case TOKEN_SCOPE:
        case TOKEN_VARIADIC:
        case TOKEN_LPAREN:
        case TOKEN_RPAREN:
        case TOKEN_LBRACE:
        case TOKEN_RBRACE:
        case TOKEN_LBRACKET:
        case TOKEN_RBRACKET:
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
        case TOKEN_BNOT:
        case TOKEN_SET:
        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
        case TOKEN_MOD_SET:
        case TOKEN_SHL_SET:
        case TOKEN_SHR_SET:
        case TOKEN_BOR_SET:
        case TOKEN_BAND_SET:
        case TOKEN_LOR:
        case TOKEN_LAND:
        case TOKEN_LNOT:
        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            // Poss
            break;

        case TOKEN_IDENT: {
            static const char *types[] = {
                "bool",
                "char",
                "i8",
                "i16",
                "i32",
                "i64",
                "u8",
                "u16",
                "u32",
                "u64",
                "f32",
                "f64",
                "rawptr",
            };

            static const char *keywords[] = {
                "sizeof", "assert", "if",       "then", "else",   "for",     "match", "defer",
                "break",  "return", "continue", "fn",   "var",    "type",    "const", "trait",
                "struct", "extern", "static",   "pub",  "import", "package", "when",
            };

            static const char *constants[] = {"nil", "true", "false"};

            if (lexer_peek(&lexer).kind == TOKEN_LPAREN) {
                style = STYLE_FN;
            } else if (find_in_list(types, len(types), token.sv)) {
                style = STYLE_TYPE;
            } else if (find_in_list(keywords, len(keywords), token.sv)) {
                style = STYLE_KEYWORD;
            } else if (find_in_list(constants, len(constants), token.sv)) {
                style = STYLE_CONSTANT;
            }
        } break;

        case TOKEN_EOL:
            style = STYLE_COMMENT;
            break;

        case TOKEN_NIL:
        case TOKEN_INT:
        case TOKEN_FLOAT:
        case TOKEN_BOOL:
            style = STYLE_CONSTANT;
            break;

        case TOKEN_STR:
        case TOKEN_CHAR:
            style = STYLE_STRING;
            break;

        case TOKEN_SIZEOF:
        case TOKEN_ASSERT:
        case TOKEN_IF:
        case TOKEN_THEN:
        case TOKEN_ELSE:
        case TOKEN_FOR:
        case TOKEN_MATCH:
        case TOKEN_DEFER:
        case TOKEN_BREAK:
        case TOKEN_RETURN:
        case TOKEN_CONTINUE:
        case TOKEN_FN:
        case TOKEN_VAR:
        case TOKEN_TYPE:
        case TOKEN_CONST:
        case TOKEN_TRAIT:
        case TOKEN_STRUCT:
        case TOKEN_EXTERN:
        case TOKEN_STATIC:
        case TOKEN_PUB:
        case TOKEN_IMPORT:
        case TOKEN_PACKAGE:
        case TOKEN_WHEN:
            style = STYLE_KEYWORD;
            break;

        case TOKEN_PROP_OS:
        case TOKEN_PROP_LINK:
            style = STYLE_CONSTANT;
            break;

        case COUNT_TOKENS:
            unreachable();
            break;
        }

        const SV before = {
            .data = last,
            .count = token.sv.data - last,
        };

        if (before.count) {
            docs_printf_safe(d, STYLE_COMMENT, SVFmt, SVArg(before));
        }
        last = token.sv.data + token.sv.count;

        if (token.kind == TOKEN_EOF) {
            break;
        }
        docs_printf_safe(d, style, SVFmt, SVArg(token.sv));
    }
    fprintf(d->f, "</pre>\n");
}

static void docs_emit_comments(Docs *d, long start) {
    Pos  codeblock_pos = {0};
    bool codeblock_started = false;
    bool paragraph_started = false;
    if (start != -1) {
        size_t i = 0, j = start;
        size_t k = d->comments->data[j].pos.row;
        while (j < d->comments->count && d->comments->data[j].pos.row == k + i) {
            SV   sv = d->comments->data[j].sv;
            bool skipped = false;
            if (sv.count && isspace(*sv.data)) {
                skipped = true;
                sv_drop(&sv, 1);
            }

            if (sv_match(sv, "```")) {
                if (codeblock_started) {
                    codeblock_started = false;
                    docs_emit_code(d, codeblock_pos);
                } else {
                    if (paragraph_started) {
                        fprintf(d->f, "</p>\n");
                        paragraph_started = false;
                    }

                    codeblock_started = true;

                    assert(j + 1 < d->comments->count);
                    codeblock_pos = d->comments->data[j + 1].pos;
                    codeblock_pos.col += 2 + skipped;

                    d->sb.count = 0;
                }
            } else if (codeblock_started) {
                sb_push_many(&d->sb, sv.data, sv.count);
                sb_push(&d->sb, '\n');
            } else {
                if (sv.count) {
                    if (!paragraph_started) {
                        fprintf(d->f, "<p>\n");
                        paragraph_started = true;
                    }

                    docs_printf_safe(d, STYLE_NORMAL, SVFmt "\n", SVArg(sv));
                    fprintf(d->f, "<br>\n");
                } else {
                    if (paragraph_started) {
                        fprintf(d->f, "</p>\n");
                        paragraph_started = false;
                    }
                }
            }

            i++;
            j++;
        }
    }

    if (paragraph_started) {
        fprintf(d->f, "</p>\n");
    }
}

static void create_dir(const char *path) {
    const size_t total = strlen(path);
    const char  *p = path;
    if (*p == '/') {
        p++;
    }

    while ((p = memchr(p, '/', total - (p - path)))) {
        const char *segment = temp_sv_to_cstr((SV) {path, p - path});
        if (mkdir(segment, 0755) < 0) {
            if (errno != EEXIST) {
                error_standalone(ERROR, "Could not create directory '%s'", segment);
                exit(1);
            }
        }
        temp_reset(segment);
        p++;
    }

    if (mkdir(path, 0755) < 0) {
        if (errno != EEXIST) {
            error_standalone(ERROR, "Could not create directory '%s'", path);
            exit(1);
        }
    }
}

static void copy_asset_maybe(Docs *d, const char *name) {
    const void *checkpoint = temp_alloc(0);

    const char *src = temp_sprintf(SVFmt "/../assets/%s", SVArg(d->std), name);
    const char *dst = temp_sprintf(SVFmt "/%s", SVArg(d->dir), name);

    const int dst_time = get_modified_time(dst);
    const int src_time = get_modified_time(src);
    if (dst_time == -1 || dst_time < src_time) {
        if (!copy_file(dst, src)) {
            error_standalone(ERROR, "Could not copy documentation asset '%s' to '" SVFmt "'", name, SVArg(d->dir));
            exit(1);
        }
    }

    temp_reset(checkpoint);
}

static void docs_init(Docs *d, const char *dir, const char *std) {
    d->dir = sv_from_cstr(dir);
    d->std = sv_from_cstr(std);
    create_dir(dir);

    const void *checkpoint = temp_alloc(0);
    copy_asset_maybe(d, "index.js");
    copy_asset_maybe(d, "index.css");

    create_dir(temp_sprintf("%s/fonts/JetBrainsMono", dir));
    copy_asset_maybe(d, "fonts/JetBrainsMono/JetBrainsMono-Regular.ttf");
    copy_asset_maybe(d, "fonts/JetBrainsMono/OFL.txt");

    create_dir(temp_sprintf("%s/fonts/Roboto", dir));
    copy_asset_maybe(d, "fonts/Roboto/LICENSE.txt");
    copy_asset_maybe(d, "fonts/Roboto/Roboto-Regular.ttf");

    temp_reset(checkpoint);
}

static void docs_finalize(Docs *d) {
    const char *homepage = temp_sprintf(SVFmt "/index.html", SVArg(d->dir));

    FILE *f = fopen(homepage, "w");
    if (!f) {
        error_standalone(ERROR, "ERROR: Could not create documentation homepage '%s'", homepage);
        exit(1);
    }

    fprintf(
        f,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<link rel='stylesheet' href='/index.css'>\n"
        "<title>Packages</title>\n"
        "</head>\n"
        "<body>\n"
        "<h1>Packages</h1>\n"
        "<ul>\n");

    da_foreach(d->packages, it) {
        const SV path = docs_package_path(*it);
        fprintf(f, "<li><a href='" SVFmt "'>" SVFmt "</a></li>\n", SVArg(path), SVArg(*it));
    }

    fprintf(
        f,
        "</ul>\n"
        "</body>\n"
        "</html>\n");

    fclose(f);
    temp_reset(homepage);

    printf("Generated documentation into '" SVFmt "'\n", SVArg(d->dir));
    da_free(&d->packages);
    sb_free(&d->sb);
}

static void docs_emit_package(Docs *d, Package *p) {
    if (p->is_documented) {
        return;
    }
    p->is_documented = true;

    if (sv_has_prefix(p->relative_path, sv_from_cstr(".."))) {
        error_standalone(
            ERROR,
            "Package '" SVFmt "' (" SVFmt ") exists outside working directory",
            SVArg(p->name.sv),
            SVArg(p->relative_path));

        exit(1);
    }

    if (sv_has_prefix(p->absolute_path, d->std) &&
        (p->absolute_path.count == d->std.count || p->absolute_path.data[d->std.count] == '/')) {
        p->relative_path = p->absolute_path;
        sv_drop(&p->relative_path, d->std.count);
        if (p->relative_path.count) {
            sv_drop(&p->relative_path, 1);
        }
    }

    da_push(&d->packages, p->relative_path);
    d->current = p;

    const char *output = NULL;
    {
        SV path = docs_package_path(p->relative_path);

        const char *parent = temp_sprintf(SVFmt "/" SVFmt, SVArg(d->dir), SVArg(path));
        create_dir(parent);
        temp_reset(parent);
        output = temp_sprintf(SVFmt "/" SVFmt "/index.html", SVArg(d->dir), SVArg(path));
    }

    if (get_modified_time(output) > p->modified_time) {
        temp_reset(output);
        return;
    }

    d->f = fopen(output, "w");
    if (!d->f) {
        error_standalone(ERROR, "Could not create documentation file '%s'", output);
        exit(1);
    }
    temp_reset(output);

    fprintf(
        d->f,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<link rel='stylesheet' href='/index.css'>\n"
        "<script src='/index.js'></script>\n"
        "<title>Package " SVFmt "</title>\n"
        "</head>\n"
        "<body>\n",
        SVArg(p->name.sv));

    fprintf(d->f, "<h1>Package " SVFmt "</h1>\n", SVArg(p->name.sv));

    if (!sv_match(p->relative_path, ".") && !p->is_builtin) {
        fprintf(d->f, "<pre>\n");
        docs_printf_safe(d, STYLE_KEYWORD, "import ");
        docs_format_style_begin(d, STYLE_STRING);
        docs_printf_safe(d, STYLE_NORMAL, "\"");
        for (size_t i = 0; i < p->relative_path.count; i++) {
            print_quoted_char(d->f, p->relative_path.data[i], '"');
        }
        docs_printf_safe(d, STYLE_NORMAL, "\"\n");
        docs_format_style_end(d, STYLE_STRING);
        fprintf(d->f, "</pre>\n");
    }

    {
        bool header_printed = false;
        for (size_t i = 0; i < p->globals.count; i++) {
            Node *it = p->globals.data[i];
            if (it->kind == NODE_CONST) {
                if (!header_printed) {
                    header_printed = true;
                    fprintf(d->f, "<h2>Constants</h2>\n");
                }

                fprintf(d->f, "<pre id='" SVFmt "'>\n", SVArg(it->token.sv));
                docs_emit_const(d, it);
                fprintf(d->f, "</pre>\n");
                docs_emit_comments(d, it->fmt_doc_comment_start);
            }
        }
    }

    {
        bool header_printed = false;
        for (size_t i = 0; i < p->globals.count; i++) {
            Node *it = p->globals.data[i];
            if (it->kind == NODE_TYPE || it->kind == NODE_TRAIT || it->kind == NODE_STRUCT) {
                if (!header_printed) {
                    header_printed = true;
                    fprintf(d->f, "<h2>Types</h2>\n");
                }

                fprintf(d->f, "<pre id='@" SVFmt "'>\n", SVArg(it->token.sv));
                docs_emit_type(d, it);
                fprintf(d->f, "</pre>\n");
                docs_emit_comments(d, it->fmt_doc_comment_start);
            }
        }
    }

    {
        bool header_printed = false;
        for (size_t i = 0; i < p->globals.count; i++) {
            Node *it = p->globals.data[i];
            if (it->kind == NODE_VAR) {
                if (!header_printed) {
                    header_printed = true;
                    fprintf(d->f, "<h2>Variables</h2>\n");
                }

                fprintf(d->f, "<pre id='" SVFmt "'>\n", SVArg(it->token.sv));
                docs_emit_var(d, it);
                fprintf(d->f, "</pre>\n");
                docs_emit_comments(d, it->fmt_doc_comment_start);
            }
        }
    }

    {
        bool header_printed = false;
        for (size_t i = 0; i < p->globals.count; i++) {
            Node *it = p->globals.data[i];
            if (it->kind == NODE_FN) {
                if (!header_printed) {
                    header_printed = true;
                    fprintf(d->f, "<h2>Functions</h2>\n");
                }

                fprintf(d->f, "<pre id='" SVFmt "'>\n", SVArg(it->token.sv));
                docs_emit_fn(d, it);
                fprintf(d->f, "</pre>\n");
                docs_emit_comments(d, it->fmt_doc_comment_start);
            }
        }
    }

    {
        bool header_printed = false;
        for (Node *it = p->nodes.head; it; it = it->next) {
            if (it->kind == NODE_FN && ((NodeFn *) it)->is_method) {
                if (!header_printed) {
                    header_printed = true;
                    fprintf(d->f, "<h2>Methods</h2>\n");
                }

                fprintf(d->f, "<pre id='" SVFmt "'>\n", SVArg(it->token.sv));
                docs_emit_fn(d, it);
                fprintf(d->f, "</pre>\n");
                docs_emit_comments(d, it->fmt_doc_comment_start);
            }
        }
    }

    fprintf(
        d->f,
        "</body>\n"
        "</html>\n");

    fclose(d->f);
}

void docs_generate(Packages ps, Comments *comments, const char *dir, const char *std) {
    Docs d = {0};
    docs_init(&d, dir, std);

    d.comments = comments;
    for (Package *it = ps.head; it; it = it->next) {
        docs_emit_package(&d, it);
    }

    docs_finalize(&d);
}
