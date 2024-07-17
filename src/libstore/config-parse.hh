#pragma once

/**
 * Look up the setting's name in a map, falling back on the default if
 * it does not exist.
 */
#define CONFIG_ROW(FIELD)                                  \
    .FIELD = {                                             \
        .value = ({                                        \
            auto p = get(params, descriptions.FIELD.name); \
            p ? *p : defaults.FIELD.value;                 \
        })                                                 \
    }

