[ "${input1: -2}" = /. ]
[ "${input2: -2}" = /. ]

mkdir $out
echo $(cat $input1/foo)$(cat $input2/bar) > $out/foobar

echo $input2 > $out/input-2

# Self-reference.
echo $out > $out/self

# Executable.
echo program > $out/program
chmod +x $out/program

echo FOO
