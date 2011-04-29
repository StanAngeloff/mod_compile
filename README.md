mod_compile
===========

An on-the-fly compilation utility for Apache 2 with planned support for caching and proper headers.

**WORK IN PROGRESS. It doesn't do anything at the moment.**

Example
-------

Server configuration:

    Compile On
    AddCompileCommand '/usr/local/bin/coffee -cp %s' .coffee
    AddCompileCommand '/usr/bin/sass %s'             .scss

HTML page:

    <script src=/path/to/file.coffee></script>
    <link rel=stylesheet href=/path/to/style.scss>

and voila! `.coffee` files will be served as compiled JavaScript and `.scss` files as compiled CSS.

Why?
----

Native solution (no dependency on Ruby, Perl or what not), speed, ease-of-use and mostly because I wanted to learn how to write an Apache module.

Install
-------

    apxs2 -D _DEBUG -cia mod_compile.c
    apachectl2 -k restart
