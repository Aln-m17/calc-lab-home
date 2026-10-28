calc: calc.c
	gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm

san: calc.c
	gcc -O1 -g -Wall -Wextra -std=c17 -fsanitize=address,undefined -o calc calc.c -lm

test: calc
	sh calc-tests/run_tests.sh ./calc
	sh calc-tests/cli.sh ./calc

.PHONY: san test
