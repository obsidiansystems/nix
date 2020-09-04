with import ./.;

{
   inherit (hydraJobs.build)
     x86_64-linux
     x86_64-darwin
     ;
   linuxStatic = hydraJobs.build-static.x86_64-linux;
}
