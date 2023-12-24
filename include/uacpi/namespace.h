#pragma once

#include <uacpi/types.h>
#include <uacpi/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uacpi_namespace_node uacpi_namespace_node;

uacpi_namespace_node *uacpi_namespace_root(void);

enum uacpi_predefined_namespace {
    UACPI_PREDEFINED_NAMESPACE_ROOT = 0,
    UACPI_PREDEFINED_NAMESPACE_GPE,
    UACPI_PREDEFINED_NAMESPACE_PR,
    UACPI_PREDEFINED_NAMESPACE_SB,
    UACPI_PREDEFINED_NAMESPACE_SI,
    UACPI_PREDEFINED_NAMESPACE_TZ,
    UACPI_PREDEFINED_NAMESPACE_GL,
    UACPI_PREDEFINED_NAMESPACE_OS,
    UACPI_PREDEFINED_NAMESPACE_OSI,
    UACPI_PREDEFINED_NAMESPACE_REV,
    UACPI_PREDEFINED_NAMESPACE_MAX = UACPI_PREDEFINED_NAMESPACE_REV,
};
uacpi_namespace_node *uacpi_namespace_get_predefined(
    enum uacpi_predefined_namespace
);

uacpi_object *uacpi_namespace_node_get_object(uacpi_namespace_node *node);

uacpi_namespace_node *uacpi_namespace_node_find(
    uacpi_namespace_node *parent,
    const uacpi_char *path
);

#ifdef __cplusplus
}
#endif