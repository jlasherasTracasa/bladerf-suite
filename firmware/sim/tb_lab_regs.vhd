-- tb_lab_regs.vhd — Prueba del banco de registros Wishbone con el wishbone_master
-- REAL de Nuand (el que cuelga del Nios), emulando los accesos Avalon del Nios, y
-- del cruce de reloj hacia el dominio de RX.
library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity tb_lab_regs is
end entity;

architecture sim of tb_lab_regs is
    signal clk, rxclk : std_logic := '0';
    signal rst : std_logic := '1';

    -- Avalon (lado Nios)
    signal av_addr  : std_logic_vector(23 downto 0) := (others => '0');
    signal av_din, av_dout : std_logic_vector(31 downto 0) := (others => '0');
    signal av_read, av_write, av_wait, av_rvalid, av_intr : std_logic := '0';

    -- Wishbone
    signal wb_adr, wb_dat_o, wb_dat_i : std_logic_vector(31 downto 0);
    signal wb_we, wb_sel, wb_stb, wb_ack, wb_cyc : std_logic;

    signal cfg_log2n : unsigned(3 downto 0);
    signal cfg_k : unsigned(15 downto 0);
    signal cfg_pad : std_logic;
    signal cfg_clear : unsigned(7 downto 0);
    signal rx_cfg : std_logic_vector(28 downto 0);
    signal rx_log2n : unsigned(3 downto 0);
    signal rx_k : unsigned(15 downto 0);
    signal rx_pad : std_logic;
    signal rx_clear : unsigned(7 downto 0);

    signal cnt_rx : std_logic_vector(192 downto 0);
    signal cnt_sys : std_logic_vector(192 downto 0);
    signal rx_counter : unsigned(31 downto 0) := (others => '0');

    signal fails : natural := 0;
begin
    clk   <= not clk after 6.25 ns;   -- 80 MHz
    rxclk <= not rxclk after 4 ns;    -- 125 MHz
    rst <= '1', '0' after 100 ns;

    u_wbm : entity work.wishbone_master
        generic map (ADDR_BITS => 32, DATA_BITS => 32)
        port map (
            clock => clk, reset => rst,
            addr => x"00" & av_addr, din => av_din, dout => av_dout,
            write => av_write, read => av_read, waitreq => av_wait, readack => av_rvalid, intr => av_intr,
            wb_clk_i => clk, wb_rst_i => rst,
            wb_adr_o => wb_adr, wb_dat_o => wb_dat_o, wb_dat_i => wb_dat_i,
            wb_we_o => wb_we, wb_sel_o => wb_sel, wb_stb_o => wb_stb, wb_ack_i => wb_ack, wb_cyc_o => wb_cyc
        );

    u_regs : entity work.lab_regs
        port map (
            clk => clk, rst => rst,
            wb_cyc => wb_cyc, wb_adr => wb_adr, wb_dat_w => wb_dat_o, wb_we => wb_we,
            wb_dat_r => wb_dat_i, wb_ack => wb_ack,
            cfg_log2n => cfg_log2n, cfg_k => cfg_k, cfg_pad => cfg_pad, cfg_clear => cfg_clear,
            c_samples => unsigned(cnt_sys(31 downto 0)), c_ovf => unsigned(cnt_sys(63 downto 32)),
            c_fft => unsigned(cnt_sys(95 downto 64)), c_spec => unsigned(cnt_sys(127 downto 96)),
            c_late => unsigned(cnt_sys(159 downto 128)), c_words => unsigned(cnt_sys(191 downto 160)),
            st_active => cnt_sys(192)
        );

    -- Cruce sys -> rx (configuración)
    u_cfg : entity work.lab_cdc_bus
        generic map (W => 29)
        port map (src_clk => clk, src_rst => rst, src_data => std_logic_vector(cfg_clear) & cfg_pad & std_logic_vector(cfg_k) & std_logic_vector(cfg_log2n),
                  dst_clk => rxclk, dst_rst => rst, dst_data => rx_cfg);
    rx_log2n <= unsigned(rx_cfg(3 downto 0));
    rx_k     <= unsigned(rx_cfg(19 downto 4));
    rx_pad   <= rx_cfg(20);
    rx_clear <= unsigned(rx_cfg(28 downto 21));

    -- Cruce rx -> sys (contadores): el contador de prueba corre en rxclk
    cnt_rx <= '1' & x"00000000" & x"00000000" & x"00000000" & x"00000000" & x"00000000" & std_logic_vector(rx_counter);
    u_cnt : entity work.lab_cdc_bus
        generic map (W => 193)
        port map (src_clk => rxclk, src_rst => rst, src_data => cnt_rx,
                  dst_clk => clk, dst_rst => rst, dst_data => cnt_sys);
    process(rxclk) begin
        if rising_edge(rxclk) then rx_counter <= rx_counter + 1; end if;
    end process;

    stim : process
        procedure avwrite(idx : natural; v : std_logic_vector(31 downto 0)) is
        begin
            wait until rising_edge(clk);
            av_addr <= std_logic_vector(to_unsigned(idx, 24)); av_din <= v; av_write <= '1';
            wait until rising_edge(clk);
            while av_wait = '1' loop wait until rising_edge(clk); end loop;
            av_write <= '0';
            -- esperar a que el maestro termine (waitrequest vuelve a 0 tras el ack)
            for i in 0 to 3 loop wait until rising_edge(clk); end loop;
            while av_wait = '1' loop wait until rising_edge(clk); end loop;
        end procedure;
        procedure avread(idx : natural; v : out std_logic_vector(31 downto 0)) is
            variable t : natural := 0;
        begin
            wait until rising_edge(clk);
            av_addr <= std_logic_vector(to_unsigned(idx, 24)); av_read <= '1';
            wait until rising_edge(clk);
            while av_wait = '1' loop wait until rising_edge(clk); end loop;
            av_read <= '0';
            while av_rvalid = '0' loop
                wait until rising_edge(clk);
                t := t + 1;
                assert t < 200 report "TIMEOUT: el maestro Wishbone no devolvio dato (cuelgue)" severity failure;
            end loop;
            v := av_dout;
            for i in 0 to 3 loop wait until rising_edge(clk); end loop;
        end procedure;
        variable r : std_logic_vector(31 downto 0);
        procedure expect(name : string; got, exp : std_logic_vector(31 downto 0)) is
        begin
            if got /= exp then
                report "FALLO " & name & ": leido " & to_hstring(got) & " esperado " & to_hstring(exp) severity error;
                fails <= fails + 1;
            else
                report "ok " & name & " = " & to_hstring(got);
            end if;
        end procedure;
    begin
        wait for 300 ns;
        avread(0, r);  expect("ID", r, x"4C414231");
        avread(1, r);  expect("VERSION", r, x"00010001");
        avread(2, r);  expect("FEATURES", r, x"00000DA1");
        avwrite(3, x"A5C3F00D");
        avread(3, r);  expect("SCRATCH", r, x"A5C3F00D");
        avread(4, r);  expect("SCRATCH_N", r, x"5A3C0FF2");
        avread(99, r); expect("no mapeado (responde igual)", r, x"BAD0BAD0");
        avread(5, r);  expect("LOG2N defecto", r, x"0000000C");
        avwrite(5, x"0000000B");
        avwrite(6, x"00000040");
        avwrite(7, x"00000000");
        avread(5, r);  expect("LOG2N", r, x"0000000B");
        avread(6, r);  expect("K", r, x"00000040");
        avread(7, r);  expect("CTRL", r, x"00000000");
        wait for 2 us;
        assert rx_log2n = 11 and rx_k = 64 and rx_pad = '0' report "FALLO cruce de configuracion a rx" severity error;
        if not (rx_log2n = 11 and rx_k = 64 and rx_pad = '0') then fails <= fails + 1; end if;
        avwrite(15, x"00000001");
        avread(15, r); expect("CLEAR gen", r, x"00000001");
        wait for 2 us;
        assert rx_clear = 1 report "FALLO cruce del borrado" severity error;
        avread(8, r);
        report "CNT_SAMPLES (contador de rx_clock cruzado) = " & integer'image(to_integer(unsigned(r)));
        assert unsigned(r) > 100 report "FALLO: el contador cruzado no avanza" severity error;
        avread(14, r); expect("STATUS", r, x"00000001");
        wait for 1 us;
        if fails = 0 then report "TB_LAB_REGS: OK"; else report "TB_LAB_REGS: FALLO" severity error; end if;
        std.env.stop;
        wait;
    end process;
end architecture;
