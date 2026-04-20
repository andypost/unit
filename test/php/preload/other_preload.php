<?php
/* other_preload.php — alternate preload, should NOT win when Unit's `preload`
 * key is set alongside it via options.file. Defines DIFFERENT symbols so the
 * happy-path symbols from hello_preload.php are the ones observed. */

class OtherPreloaded
{
}

function other_preloaded_fn()
{
    return 7;
}
