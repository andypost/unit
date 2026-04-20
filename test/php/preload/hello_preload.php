<?php

class HelloPreloaded
{
    public function greet()
    {
        return 'hello';
    }
}

function preloaded_fn()
{
    return 42;
}
