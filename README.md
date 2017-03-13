# cpaste

cpaste is a light web application that functions in an identical manner to [sprunge](http://github.com/rupa/sprunge).

It is written in C and built with [Kore](https://github.com/jorisvink/kore).

# running

Kore applications are run as a standalone web server. Though they can support SSL, _I_ am of the opinion that it should 
be served by a reverse proxy if you're going to run this on the public web. Maybe you use SSL between your applications 
and your reverse proxy. I'm not judging.

_Before building cpaste, be sure that the inih source code is present (`git submodule update --init --recursive`)._

You'll need kore installed to build this, but that should go without saying.

    1. Put build in production mode `kodev flavor prod` 

    2. Build cpaste `kodev build`

    3. Make changes to [cpaste.ini](conf/cpaste.ini)

    4. Set `CPASTE_CONFIG_FILE` to the location of your configuration file (or place it in the directory cpaste will run in)

    5. Run cpaste

